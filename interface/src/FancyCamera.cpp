//
//  FancyCamera.cpp
//  interface/src
//
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "FancyCamera.h"

#include "Application.h"
#include "avatar/AvatarManager.h"

FancyCamera::FancyCamera() : Camera() {
  QObject::connect(this, &FancyCamera::parentIDChanged, this, [this]{
    bool success;
    _parent = SpatiallyNestable::findByID(parentID, success);
  });
}

PickRay FancyCamera::computePickRay(float x, float y) const {
    return qApp->computePickRay(x, y);
}
