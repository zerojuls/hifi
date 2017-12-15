//
//  Created by Andrew Meadows 2017.01.17
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "CauterizedModel.h"

#include <PerfStat.h>
#include <DualQuaternion.h>

#include "AbstractViewStateInterface.h"
#include "MeshPartPayload.h"
#include "CauterizedMeshPartPayload.h"
#include "RenderUtilsLogging.h"


CauterizedModel::CauterizedModel(QObject* parent) :
        Model(parent) {
}

CauterizedModel::~CauterizedModel() {
}

void CauterizedModel::deleteGeometry() {
    Model::deleteGeometry();
    _cauterizeMeshStates.clear();
}

bool CauterizedModel::updateGeometry() {
    bool needsFullUpdate = Model::updateGeometry();
    if (_isCauterized && needsFullUpdate) {
        assert(_cauterizeMeshStates.empty());
        const FBXGeometry& fbxGeometry = getFBXGeometry();
        foreach (const FBXMesh& mesh, fbxGeometry.meshes) {
            Model::MeshState state;
            state.clusterMatrices.resize(mesh.clusters.size());
            _cauterizeMeshStates.append(state);
        }
    }
    return needsFullUpdate;
}

void CauterizedModel::createVisibleRenderItemSet() {
    if (_isCauterized) {
        assert(isLoaded());
        const auto& meshes = _renderGeometry->getMeshes();

        // all of our mesh vectors must match in size
        if (meshes.size() != _meshStates.size()) {
            qCDebug(renderutils) << "WARNING!!!! Mesh Sizes don't match! We will not segregate mesh groups yet.";
            return;
        }

        // We should not have any existing renderItems if we enter this section of code
        Q_ASSERT(_modelMeshRenderItems.isEmpty());

        _modelMeshRenderItems.clear();
        _modelMeshRenderItemShapes.clear();

        Transform transform;
        transform.setTranslation(_translation);
        transform.setRotation(_rotation);

        Transform offset;
        offset.setScale(_scale);
        offset.postTranslate(_offset);

        // Run through all of the meshes, and place them into their segregated, but unsorted buckets
        int shapeID = 0;
        uint32_t numMeshes = (uint32_t)meshes.size();
        for (uint32_t i = 0; i < numMeshes; i++) {
            const auto& mesh = meshes.at(i);
            if (!mesh) {
                continue;
            }

            // Create the render payloads
            int numParts = (int)mesh->getNumParts();
            for (int partIndex = 0; partIndex < numParts; partIndex++) {
                auto ptr = std::make_shared<CauterizedMeshPartPayload>(shared_from_this(), i, partIndex, shapeID, transform, offset);
                _modelMeshRenderItems << std::static_pointer_cast<ModelMeshPartPayload>(ptr);
                _modelMeshRenderItemShapes.emplace_back(ShapeInfo{ (int)i });
                shapeID++;
            }
        }
    } else {
        Model::createVisibleRenderItemSet();
    }
}

void CauterizedModel::createCollisionRenderItemSet() {
    // Temporary HACK: use base class method for now
    Model::createCollisionRenderItemSet();
}

void CauterizedModel::updateClusterMatrices() {
    PerformanceTimer perfTimer("CauterizedModel::updateClusterMatrices");

    if (!_needsUpdateClusterMatrices || !isLoaded()) {
        return;
    }
    _needsUpdateClusterMatrices = false;
    const FBXGeometry& geometry = getFBXGeometry();

    for (int i = 0; i < (int)_meshStates.size(); i++) {
        Model::MeshState& state = _meshStates[i];
        const FBXMesh& mesh = geometry.meshes.at(i);
        for (int j = 0; j < mesh.clusters.size(); j++) {
            const FBXCluster& cluster = mesh.clusters.at(j);

            /* AJT: TODO REMOVE
            auto jointMatrix = _rig.getJointTransform(cluster.jointIndex);
            glm_mat4u_mul(jointMatrix, cluster.inverseBindMatrix, state.clusterMatrices[j]);
            */
            // AJT: TODO OPTOMIZE
            AnimPose jointPose = _rig.getJointPose(cluster.jointIndex);
            AnimPose result = jointPose * AnimPose(cluster.inverseBindMatrix);

            // pack scale rotation and translation into a mat4.
            state.clusterMatrices[j][0].x = result.scale().x;
            state.clusterMatrices[j][0].y = result.scale().y;
            state.clusterMatrices[j][0].z = result.scale().z;

            DualQuaternion dq(result.rot(), result.trans());
            state.clusterMatrices[j][1].x = dq.real().x;
            state.clusterMatrices[j][1].y = dq.real().y;
            state.clusterMatrices[j][1].z = dq.real().z;
            state.clusterMatrices[j][1].w = dq.real().w;
            state.clusterMatrices[j][2].x = dq.imag().x;
            state.clusterMatrices[j][2].y = dq.imag().y;
            state.clusterMatrices[j][2].z = dq.imag().z;
            state.clusterMatrices[j][2].w = dq.imag().w;
        }
    }

    // as an optimization, don't build cautrizedClusterMatrices if the boneSet is empty.
    if (!_cauterizeBoneSet.empty()) {

        static const glm::mat4 zeroScale(
            glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
            glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
            glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
            glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        auto cauterizeMatrix = _rig.getJointTransform(geometry.neckJointIndex) * zeroScale;
        auto cauterizePose = AnimPose(cauterizeMatrix);

        for (int i = 0; i < _cauterizeMeshStates.size(); i++) {
            Model::MeshState& state = _cauterizeMeshStates[i];
            const FBXMesh& mesh = geometry.meshes.at(i);
            for (int j = 0; j < mesh.clusters.size(); j++) {
                const FBXCluster& cluster = mesh.clusters.at(j);

                // AJT: TODO REMOVE:
                /*
                auto jointMatrix = _rig.getJointTransform(cluster.jointIndex);
                if (_cauterizeBoneSet.find(cluster.jointIndex) != _cauterizeBoneSet.end()) {
                    jointMatrix = cauterizeMatrix;
                }
                glm_mat4u_mul(jointMatrix, cluster.inverseBindMatrix, state.clusterMatrices[j]);
                */

                auto jointPose = _rig.getJointPose(cluster.jointIndex);
                if (_cauterizeBoneSet.find(cluster.jointIndex) != _cauterizeBoneSet.end()) {
                    jointPose = cauterizePose;
                }
                AnimPose result = jointPose * AnimPose(cluster.inverseBindMatrix);

                // pack scale rotation and translation into a mat4.
                state.clusterMatrices[j][0].x = result.scale().x;
                state.clusterMatrices[j][0].y = result.scale().y;
                state.clusterMatrices[j][0].z = result.scale().z;

                DualQuaternion dq(result.rot(), result.trans());
                state.clusterMatrices[j][1].x = dq.real().x;
                state.clusterMatrices[j][1].y = dq.real().y;
                state.clusterMatrices[j][1].z = dq.real().z;
                state.clusterMatrices[j][1].w = dq.real().w;
                state.clusterMatrices[j][2].x = dq.imag().x;
                state.clusterMatrices[j][2].y = dq.imag().y;
                state.clusterMatrices[j][2].z = dq.imag().z;
                state.clusterMatrices[j][2].w = dq.imag().w;
            }
        }
    }

    // post the blender if we're not currently waiting for one to finish
    if (geometry.hasBlendedMeshes() && _blendshapeCoefficients != _blendedBlendshapeCoefficients) {
        _blendedBlendshapeCoefficients = _blendshapeCoefficients;
        DependencyManager::get<ModelBlender>()->noteRequiresBlend(getThisPointer());
    }
}

void CauterizedModel::updateRenderItems() {
    if (_isCauterized) {
        if (!_addedToScene) {
            return;
        }

        glm::vec3 scale = getScale();
        if (_collisionGeometry) {
            // _collisionGeometry is already scaled
            scale = glm::vec3(1.0f);
        }
        _needsUpdateClusterMatrices = true;
        _renderItemsNeedUpdate = false;

        // queue up this work for later processing, at the end of update and just before rendering.
        // the application will ensure only the last lambda is actually invoked.
        void* key = (void*)this;
        std::weak_ptr<CauterizedModel> weakSelf = std::dynamic_pointer_cast<CauterizedModel>(shared_from_this());
        AbstractViewStateInterface::instance()->pushPostUpdateLambda(key, [weakSelf]() {
            // do nothing, if the model has already been destroyed.
            auto self = weakSelf.lock();
            if (!self || !self->isLoaded()) {
                return;
            }

            // lazy update of cluster matrices used for rendering.  We need to update them here, so we can correctly update the bounding box.
            self->updateClusterMatrices();

            render::ScenePointer scene = AbstractViewStateInterface::instance()->getMain3DScene();

            Transform modelTransform;
            modelTransform.setTranslation(self->getTranslation());
            modelTransform.setRotation(self->getRotation());

            render::Transaction transaction;
            for (int i = 0; i < (int)self->_modelMeshRenderItemIDs.size(); i++) {

                auto itemID = self->_modelMeshRenderItemIDs[i];
                auto meshIndex = self->_modelMeshRenderItemShapes[i].meshIndex;
                auto clusterMatrices(self->getMeshState(meshIndex).clusterMatrices);
                auto clusterMatricesCauterized(self->getCauterizeMeshState(meshIndex).clusterMatrices);

                transaction.updateItem<CauterizedMeshPartPayload>(itemID, [modelTransform, clusterMatrices, clusterMatricesCauterized](CauterizedMeshPartPayload& data) {
                    data.updateClusterBuffer(clusterMatrices, clusterMatricesCauterized);

                    Transform renderTransform = modelTransform;
                    if (clusterMatrices.size() == 1) {
                        renderTransform = modelTransform.worldTransform(Transform(clusterMatrices[0]));
                    }
                    data.updateTransformForSkinnedMesh(renderTransform, modelTransform);

                    renderTransform = modelTransform;
                    if (clusterMatricesCauterized.size() == 1) {
                        renderTransform = modelTransform.worldTransform(Transform(clusterMatricesCauterized[0]));
                    }
                    data.updateTransformForCauterizedMesh(renderTransform);
                });
            }

            scene->enqueueTransaction(transaction);
        });
    } else {
        Model::updateRenderItems();
    }
}

const Model::MeshState& CauterizedModel::getCauterizeMeshState(int index) const {
    assert((size_t)index < _meshStates.size());
    return _cauterizeMeshStates.at(index);
}
