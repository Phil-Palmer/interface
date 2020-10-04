//
//  CrashHandler_Crashpad.cpp
//  interface/src
//
//  Created by Clement Brisset on 01/19/18.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#if HAS_CRASHPAD

#include "CrashHandler.h"

#include <assert.h>

#include <vector>
#include <string>

#include <QtCore/QAtomicInteger>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QDateTime>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++14-extensions"
#endif

#include <client/crashpad_client.h>
#include <client/crash_report_database.h>
#include <client/settings.h>
#include <client/crashpad_info.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <BuildInfo.h>
#include <FingerprintUtils.h>
#include <UUID.h>

#include <shared/FileLogger.h>

using namespace crashpad;

static const std::string BACKTRACE_URL { CMAKE_BACKTRACE_URL };
// static const std::string BACKTRACE_TOKEN { CMAKE_BACKTRACE_TOKEN };

static constexpr DWORD STATUS_MSVC_CPP_EXCEPTION = 0xE06D7363;
static constexpr ULONG_PTR MSVC_CPP_EXCEPTION_SIGNATURE = 0x19930520;

CrashpadClient* client { nullptr };
std::mutex annotationMutex;
crashpad::SimpleStringDictionary* crashpadAnnotations { nullptr };

#if defined(Q_OS_WIN)
static const QString CRASHPAD_HANDLER_NAME { "crashpad_handler.exe" };
#else
static const QString CRASHPAD_HANDLER_NAME { "crashpad_handler" };
#endif

#ifdef Q_OS_WIN
// ------------------------------------------------------------------------------------------------
// The area within this #ifdef is specific to the Microsoft C++ compiler

static constexpr DWORD STATUS_MSVC_CPP_EXCEPTION = 0xE06D7363;
static constexpr ULONG_PTR MSVC_CPP_EXCEPTION_SIGNATURE = 0x19930520;
static constexpr int ANNOTATION_LOCK_WEAK_ATTEMPT = 5000; // attempt to lock the annotations list, but give up if it takes more than 5 seconds

#include <Windows.h>
#include <typeinfo>

void fatalCxxException(PEXCEPTION_POINTERS pExceptionInfo);

LONG WINAPI vectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    if (!client) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (pExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_HEAP_CORRUPTION ||
        pExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN) {
        client->DumpAndCrash(pExceptionInfo);
    }

    if (pExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_MSVC_CPP_EXCEPTION) {
        fatalCxxException(pExceptionInfo);
        client->DumpAndCrash(pExceptionInfo);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#pragma pack(push, ehdata, 4)

struct PMD_internal {
    int mdisp;
    int pdisp;
    int vdisp;
};

struct ThrowInfo_internal {
    __int32 attributes;
    __int32 pmfnUnwind;           // 32-bit RVA
    __int32 pForwardCompat;       // 32-bit RVA
    __int32 pCatchableTypeArray;  // 32-bit RVA
};

struct CatchableType_internal {
    __int32 properties;
    __int32 pType;                // 32-bit RVA
    PMD_internal thisDisplacement;
    __int32 sizeOrOffset;
    __int32 copyFunction;         // 32-bit RVA
};

#pragma warning(disable : 4200)
struct CatchableTypeArray_internal {
    int nCatchableTypes;
    __int32 arrayOfCatchableTypes[0]; // 32-bit RVA
};
#pragma warning(default : 4200)

#pragma pack(pop, ehdata)

// everything inside this function is extremely undocumented, attempting to extract
// the underlying C++ exception type (or at least its name) before throwing the whole
// mess at crashpad
void fatalCxxException(PEXCEPTION_POINTERS pExceptionInfo) {
    SpinLockLocker guard(crashpadAnnotationsProtect, ANNOTATION_LOCK_WEAK_ATTEMPT);
    if (!guard.isLocked()) {
        return;
    }

    PEXCEPTION_RECORD ExceptionRecord = pExceptionInfo->ExceptionRecord;
    /*
    Exception arguments for Microsoft C++ exceptions:
    [0] signature  - magic number
    [1] void*      - variable that is being thrown
    [2] ThrowInfo* - description of the variable that was thrown
    [3] HMODULE    - (64-bit only) base address that all 32bit pointers are added to
    */

    if (ExceptionRecord->NumberParameters != 4 || ExceptionRecord->ExceptionInformation[0] != MSVC_CPP_EXCEPTION_SIGNATURE) {
        // doesn't match expected parameter counts or magic numbers
        return;
    }

    // get the ThrowInfo struct from the exception arguments
    ThrowInfo_internal* pThrowInfo = reinterpret_cast<ThrowInfo_internal*>(ExceptionRecord->ExceptionInformation[2]);
    ULONG_PTR moduleBase = ExceptionRecord->ExceptionInformation[3];
    if(moduleBase == 0 || pThrowInfo == NULL) {
      return; // broken assumption
    }

    // get the CatchableTypeArray* struct from ThrowInfo
    if(pThrowInfo->pCatchableTypeArray == 0) {
      return; // broken assumption
    }
    CatchableTypeArray_internal* pCatchableTypeArray = reinterpret_cast<CatchableTypeArray_internal*>(moduleBase + pThrowInfo->pCatchableTypeArray);
    if(pCatchableTypeArray->nCatchableTypes == 0 || pCatchableTypeArray->arrayOfCatchableTypes[0] == 0) {
      return; // broken assumption
    }

    // get the CatchableType struct for the actual exception type from CatchableTypeArray
    CatchableType_internal* pCatchableType = reinterpret_cast<CatchableType_internal*>(moduleBase + pCatchableTypeArray->arrayOfCatchableTypes[0]);
    if(pCatchableType->pType == 0) {
      return; // broken assumption
    }
    const std::type_info* type = reinterpret_cast<std::type_info*>(moduleBase + pCatchableType->pType);

    crashpadAnnotations->SetKeyValue("thrownObject", type->name());

    // After annotating the name of the actual object type, go through the other entries in CatcahleTypeArray and itemize the list of possible
    // catch() commands that could have caught this so we can find the list of its superclasses
    QString compatibleObjects;
    for (int catchTypeIdx = 1; catchTypeIdx < pCatchableTypeArray->nCatchableTypes; catchTypeIdx++) {
        CatchableType_internal* pCatchableSuperclassType = reinterpret_cast<CatchableType_internal*>(moduleBase + pCatchableTypeArray->arrayOfCatchableTypes[catchTypeIdx]);
        if (pCatchableSuperclassType->pType == 0) {
          return; // broken assumption
        }
        const std::type_info* superclassType = reinterpret_cast<std::type_info*>(moduleBase + pCatchableSuperclassType->pType);

        if (!compatibleObjects.isEmpty()) {
            compatibleObjects += ", ";
        }
        compatibleObjects += superclassType->name();
    }
    crashpadAnnotations->SetKeyValue("thrownObjectLike", compatibleObjects.toStdString());
}

// End of code specific to the Microsoft C++ compiler
// ------------------------------------------------------------------------------------------------
#endif

bool startCrashHandler(std::string appPath) {
    // if (BACKTRACE_URL.empty() || BACKTRACE_TOKEN.empty()) {
    if (BACKTRACE_URL.empty()) {
        return false;
    }

    assert(!client);
    client = new crashpad::CrashpadClient();
    std::vector<std::string> arguments;

    std::map<std::string, std::string> annotations;
    annotations["sentry[release]"] = BuildInfo::VERSION.toStdString();
        
    auto machineFingerPrint = uuidStringWithoutCurlyBraces(FingerprintUtils::getMachineFingerprint());
    
    // https://develop.sentry.dev/sdk/event-payloads/contexts
    annotations["sentry[contexts][app][app_start_time]"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    // annotations["sentry[contexts][app][device_app_hash]"] = machineFingerPrint.toStdString();
    annotations["machine_fingerprint"] = machineFingerPrint.toStdString();
    annotations["sentry[contexts][app][build_type]"] = BuildInfo::BUILD_TYPE_STRING.toStdString();
    annotations["sentry[contexts][app][app_version]"] = BuildInfo::VERSION.toStdString();
    annotations["sentry[contexts][app][app_build]"] = BuildInfo::BUILD_NUMBER.toStdString();    

    arguments.push_back("--no-rate-limit");

    // // https://docs.sentry.io/platforms/native/minidump
    // std::map<std::string, std::string> fileAttachments;
    // fileAttachments["tivoli-log.txt"] = QString(FileLogger::getLogFilename()).toStdString();

    // Setup Crashpad DB directory
    const auto crashpadDbName = "crashpad-db";
    const auto crashpadDbDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir(crashpadDbDir).mkpath(crashpadDbName); // Make sure the directory exists
    const auto crashpadDbPath = crashpadDbDir.toStdString() + "/" + crashpadDbName;

    // Locate Crashpad handler
    const QFileInfo interfaceBinary { QString::fromStdString(appPath) };
    const QDir interfaceDir = interfaceBinary.dir();
    assert(interfaceDir.exists(CRASHPAD_HANDLER_NAME));
    const std::string CRASHPAD_HANDLER_PATH = interfaceDir.filePath(CRASHPAD_HANDLER_NAME).toStdString();

    // Setup different file paths
    base::FilePath::StringType dbPath;
    base::FilePath::StringType handlerPath;
    dbPath.assign(crashpadDbPath.cbegin(), crashpadDbPath.cend());
    handlerPath.assign(CRASHPAD_HANDLER_PATH.cbegin(), CRASHPAD_HANDLER_PATH.cend());

    base::FilePath db(dbPath);
    base::FilePath handler(handlerPath);

    auto database = crashpad::CrashReportDatabase::Initialize(db);
    if (database == nullptr || database->GetSettings() == nullptr) {
        return false;
    }

    // Enable automated uploads.
    database->GetSettings()->SetUploadsEnabled(true);

#ifdef Q_OS_WIN
    AddVectoredExceptionHandler(0, vectoredExceptionHandler);
#endif

    return client->StartHandler(
        handler, db, db, BACKTRACE_URL, annotations, arguments, true, true
    );
}

void setCrashAnnotation(std::string name, std::string value) {
    if (client) {
        SpinLockLocker guard(crashpadAnnotationsProtect);
        if (!crashpadAnnotations) {
            crashpadAnnotations = new crashpad::SimpleStringDictionary(); // don't free this, let it leak
            crashpad::CrashpadInfo* crashpad_info = crashpad::CrashpadInfo::GetCrashpadInfo();
            crashpad_info->set_simple_annotations(crashpadAnnotations);
        }
        std::replace(value.begin(), value.end(), ',', ';');
        crashpadAnnotations->SetKeyValue(name, value);
    }
}

#endif
