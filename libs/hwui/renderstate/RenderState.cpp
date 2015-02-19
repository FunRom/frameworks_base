/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "renderstate/RenderState.h"

#include "renderthread/CanvasContext.h"
#include "renderthread/EglManager.h"
#include "utils/GLUtils.h"

namespace android {
namespace uirenderer {

RenderState::RenderState(renderthread::RenderThread& thread)
        : mRenderThread(thread)
        , mViewportWidth(0)
        , mViewportHeight(0)
        , mFramebuffer(0) {
    mThreadId = pthread_self();
}

RenderState::~RenderState() {
    LOG_ALWAYS_FATAL_IF(mBlend || mMeshState || mScissor || mStencil,
            "State object lifecycle not managed correctly");
}

void RenderState::onGLContextCreated() {
    LOG_ALWAYS_FATAL_IF(mBlend || mMeshState || mScissor || mStencil,
            "State object lifecycle not managed correctly");
    mBlend = new Blend();
    mMeshState = new MeshState();
    mScissor = new Scissor();
    mStencil = new Stencil();

    // This is delayed because the first access of Caches makes GL calls
    if (!mCaches) {
        mCaches = &Caches::createInstance(*this);
    }
    mCaches->init();
    mCaches->textureCache.setAssetAtlas(&mAssetAtlas);
}

static void layerLostGlContext(Layer* layer) {
    layer->onGlContextLost();
}

void RenderState::onGLContextDestroyed() {
/*
    size_t size = mActiveLayers.size();
    if (CC_UNLIKELY(size != 0)) {
        ALOGE("Crashing, have %d contexts and %d layers at context destruction. isempty %d",
                mRegisteredContexts.size(), size, mActiveLayers.empty());
        mCaches->dumpMemoryUsage();
        for (std::set<renderthread::CanvasContext*>::iterator cit = mRegisteredContexts.begin();
                cit != mRegisteredContexts.end(); cit++) {
            renderthread::CanvasContext* context = *cit;
            ALOGE("Context: %p (root = %p)", context, context->mRootRenderNode.get());
            ALOGE("  Prefeteched layers: %zu", context->mPrefetechedLayers.size());
            for (std::set<RenderNode*>::iterator pit = context->mPrefetechedLayers.begin();
                    pit != context->mPrefetechedLayers.end(); pit++) {
                (*pit)->debugDumpLayers("    ");
            }
            context->mRootRenderNode->debugDumpLayers("  ");
        }


        if (mActiveLayers.begin() == mActiveLayers.end()) {
            ALOGE("set has become empty. wat.");
        }
        for (std::set<const Layer*>::iterator lit = mActiveLayers.begin();
             lit != mActiveLayers.end(); lit++) {
            const Layer* layer = *(lit);
            ALOGE("Layer %p, state %d, texlayer %d, fbo %d, buildlayered %d",
                    layer, layer->state, layer->isTextureLayer(), layer->getFbo(), layer->wasBuildLayered);
        }
        LOG_ALWAYS_FATAL("%d layers have survived gl context destruction", size);
    }
*/

    // TODO: reset all cached state in state objects
    std::for_each(mActiveLayers.begin(), mActiveLayers.end(), layerLostGlContext);
    mAssetAtlas.terminate();

    mCaches->terminate();

    delete mBlend;
    mBlend = nullptr;
    delete mMeshState;
    mMeshState = nullptr;
    delete mScissor;
    mScissor = nullptr;
    delete mStencil;
    mStencil = nullptr;
}

void RenderState::setViewport(GLsizei width, GLsizei height) {
    mViewportWidth = width;
    mViewportHeight = height;
    glViewport(0, 0, mViewportWidth, mViewportHeight);
}


void RenderState::getViewport(GLsizei* outWidth, GLsizei* outHeight) {
    *outWidth = mViewportWidth;
    *outHeight = mViewportHeight;
}

void RenderState::bindFramebuffer(GLuint fbo) {
    if (mFramebuffer != fbo) {
        mFramebuffer = fbo;
        glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    }
}

void RenderState::invokeFunctor(Functor* functor, DrawGlInfo::Mode mode, DrawGlInfo* info) {
    interruptForFunctorInvoke();
    (*functor)(mode, info);
    resumeFromFunctorInvoke();
}

void RenderState::interruptForFunctorInvoke() {
    mCaches->setProgram(nullptr);
    mCaches->textureState().resetActiveTexture();
    meshState().unbindMeshBuffer();
    meshState().unbindIndicesBuffer();
    meshState().resetVertexPointers();
    meshState().disableTexCoordsVertexArray();
    debugOverdraw(false, false);
}

void RenderState::resumeFromFunctorInvoke() {
    glViewport(0, 0, mViewportWidth, mViewportHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    debugOverdraw(false, false);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    scissor().invalidate();
    blend().invalidate();

    mCaches->textureState().activateTexture(0);
    mCaches->textureState().resetBoundTextures();
}

void RenderState::debugOverdraw(bool enable, bool clear) {
    if (mCaches->debugOverdraw && mFramebuffer == 0) {
        if (clear) {
            scissor().setEnabled(false);
            stencil().clear();
        }
        if (enable) {
            stencil().enableDebugWrite();
        } else {
            stencil().disable();
        }
    }
}

void RenderState::requireGLContext() {
    assertOnGLThread();
    mRenderThread.eglManager().requireGlContext();
}

void RenderState::assertOnGLThread() {
    pthread_t curr = pthread_self();
    LOG_ALWAYS_FATAL_IF(!pthread_equal(mThreadId, curr), "Wrong thread!");
}

class DecStrongTask : public renderthread::RenderTask {
public:
    DecStrongTask(VirtualLightRefBase* object) : mObject(object) {}

    virtual void run() override {
        mObject->decStrong(nullptr);
        mObject = nullptr;
        delete this;
    }

private:
    VirtualLightRefBase* mObject;
};

void RenderState::postDecStrong(VirtualLightRefBase* object) {
    mRenderThread.queue(new DecStrongTask(object));
}

///////////////////////////////////////////////////////////////////////////////
// Render
///////////////////////////////////////////////////////////////////////////////

/*
 * Not yet supported:
 *
 * Textures + coordinates
 * SkiaShader
 * RoundRect clipping
 */

void RenderState::render(const Glop& glop) {
    const Glop::Mesh& mesh = glop.mesh;
    const Glop::Fill& fill = glop.fill;

    // ---------------------------------------------
    // ---------- Program + uniform setup ----------
    // ---------------------------------------------
    mCaches->setProgram(fill.program);

    if (fill.colorEnabled) {
        fill.program->setColor(fill.color);
    }

    fill.program->set(glop.transform.ortho,
            glop.transform.modelView,
            glop.transform.canvas,
            glop.transform.fudgingOffset);

    // Color filter uniforms
    if (glop.fill.filterMode == ProgramDescription::kColorBlend) {
        const FloatColor& color = glop.fill.filter.color;
        glUniform4f(mCaches->program().getUniform("colorBlend"),
                color.r, color.g, color.b, color.a);
    } else if (glop.fill.filterMode == ProgramDescription::kColorMatrix) {
        glUniformMatrix4fv(mCaches->program().getUniform("colorMatrix"), 1, GL_FALSE,
                glop.fill.filter.matrix.matrix);
        glUniform4fv(mCaches->program().getUniform("colorMatrixVector"), 1,
                glop.fill.filter.matrix.vector);
    }

    // Round rect clipping uniforms
    if (glop.roundRectClipState) {
        // TODO: avoid query, and cache values (or RRCS ptr) in program
        const RoundRectClipState* state = glop.roundRectClipState;
        const Rect& innerRect = state->innerRect;
        glUniform4f(fill.program->getUniform("roundRectInnerRectLTRB"),
                innerRect.left, innerRect.top,
                innerRect.right, innerRect.bottom);
        glUniformMatrix4fv(fill.program->getUniform("roundRectInvTransform"),
                1, GL_FALSE, &state->matrix.data[0]);

        // add half pixel to round out integer rect space to cover pixel centers
        float roundedOutRadius = state->radius + 0.5f;
        glUniform1f(fill.program->getUniform("roundRectRadius"),
                roundedOutRadius);
    }
    // --------------------------------
    // ---------- Mesh setup ----------
    // --------------------------------
    // vertices
    const bool force = meshState().bindMeshBufferInternal(mesh.vertexBufferObject)
            || (mesh.vertices != nullptr);
    meshState().bindPositionVertexPointer(force, mesh.vertices, mesh.stride);

    // indices
    meshState().bindIndicesBufferInternal(mesh.indexBufferObject);

    if (mesh.vertexFlags & kTextureCoord_Attrib) {
        // glop.fill.texture always takes slot 0, shader samplers increment from there
        mCaches->textureState().activateTexture(0);

        if (glop.fill.texture.clamp != GL_INVALID_ENUM) {
            glop.fill.texture.texture->setWrap(glop.fill.texture.clamp, true);
        }
        if (glop.fill.texture.filter != GL_INVALID_ENUM) {
            glop.fill.texture.texture->setFilter(glop.fill.texture.filter, true);
        }

        mCaches->textureState().bindTexture(fill.texture.texture->id);
        meshState().enableTexCoordsVertexArray();
        meshState().bindTexCoordsVertexPointer(force, mesh.texCoordOffset, mesh.stride);
    } else {
        meshState().disableTexCoordsVertexArray();
    }
    if (mesh.vertexFlags & kColor_Attrib) {
        LOG_ALWAYS_FATAL("color vertex attribute not yet supported");
        // TODO: enable color attribute, disable when done
    }
    int alphaSlot = -1;
    if (mesh.vertexFlags & kAlpha_Attrib) {
        const void* alphaCoords = ((const GLbyte*) glop.mesh.vertices) + kVertexAlphaOffset;
        alphaSlot = fill.program->getAttrib("vtxAlpha");
        glEnableVertexAttribArray(alphaSlot);
        glVertexAttribPointer(alphaSlot, 1, GL_FLOAT, GL_FALSE, kAlphaVertexStride, alphaCoords);
    }

    // Shader uniforms
    SkiaShader::apply(*mCaches, glop.fill.skiaShaderData);

    // ------------------------------------
    // ---------- GL state setup ----------
    // ------------------------------------
    blend().setFactors(glop.blend.src, glop.blend.dst);

    // ------------------------------------
    // ---------- Actual drawing ----------
    // ------------------------------------
    if (mesh.indexBufferObject == meshState().getQuadListIBO()) {
        // Since the indexed quad list is of limited length, we loop over
        // the glDrawXXX method while updating the vertex pointer
        GLsizei elementsCount = mesh.elementCount;
        const GLbyte* vertices = static_cast<const GLbyte*>(mesh.vertices);
        while (elementsCount > 0) {
            GLsizei drawCount = MathUtils::min(elementsCount, (GLsizei) kMaxNumberOfQuads * 6);

            // rebind pointers without forcing, since initial bind handled above
            meshState().bindPositionVertexPointer(false, vertices, mesh.stride);
            if (mesh.vertexFlags & kTextureCoord_Attrib) {
                meshState().bindTexCoordsVertexPointer(false,
                        vertices + kMeshTextureOffset, mesh.stride);
            }

            glDrawElements(mesh.primitiveMode, drawCount, GL_UNSIGNED_SHORT, nullptr);
            elementsCount -= drawCount;
            vertices += (drawCount / 6) * 4 * mesh.stride;
        }
    } else if (mesh.indexBufferObject || mesh.indices) {
        glDrawElements(mesh.primitiveMode, mesh.elementCount, GL_UNSIGNED_SHORT, mesh.indices);
    } else {
        glDrawArrays(mesh.primitiveMode, 0, mesh.elementCount);
    }

    // -----------------------------------
    // ---------- Mesh teardown ----------
    // -----------------------------------
    if (glop.mesh.vertexFlags & kAlpha_Attrib) {
        glDisableVertexAttribArray(alphaSlot);
    }
}

void RenderState::dump() {
    blend().dump();
    meshState().dump();
    scissor().dump();
    stencil().dump();
}

} /* namespace uirenderer */
} /* namespace android */
