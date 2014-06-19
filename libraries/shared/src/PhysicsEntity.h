//
//  PhysicsEntity.h
//  libraries/shared/src
//
//  Created by Andrew Meadows 2014.05.30
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_PhysicsEntity_h
#define hifi_PhysicsEntity_h

#include <QVector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "CollisionInfo.h"

class Shape;
class PhysicsSimulation;

// PhysicsEntity is the base class for anything that owns one or more Shapes that collide in a 
// PhysicsSimulation.  Each CollisionInfo generated by a PhysicsSimulation has back pointers to the 
// two Shapes involved, and those Shapes may (optionally) have valid back pointers to their PhysicsEntity.

class PhysicsEntity {

public:
    PhysicsEntity();
    virtual ~PhysicsEntity();

    void setTranslation(const glm::vec3& translation);
    void setRotation(const glm::quat& rotation);

    const glm::vec3& getTranslation() const { return _translation; }
    const glm::quat& getRotation() const { return _rotation; }
    float getBoundingRadius() const { return _boundingRadius; }

    void setShapeBackPointers();

    void setEnableShapes(bool enable);

    virtual void buildShapes() = 0;
    virtual void clearShapes();
    const QVector<Shape*> getShapes() const { return _shapes; }

    PhysicsSimulation* getSimulation() const { return _simulation; }

    bool findRayIntersection(const glm::vec3& origin, const glm::vec3& direction, float& distance) const;
    bool findCollisions(const QVector<const Shape*> shapes, CollisionList& collisions);
    bool findSphereCollisions(const glm::vec3& sphereCenter, float sphereRadius, CollisionList& collisions, int skipIndex);
    bool findPlaneCollisions(const glm::vec4& plane, CollisionList& collisions);

protected:
    glm::vec3 _translation;
    glm::quat _rotation;
    float _boundingRadius;
    bool _shapesAreDirty;
    bool _enableShapes;
    QVector<Shape*> _shapes;

private:
    // PhysicsSimulation is a friend so that it can set the protected _simulation backpointer
    friend PhysicsSimulation; 
    PhysicsSimulation* _simulation;
};

#endif // hifi_PhysicsEntity_h
