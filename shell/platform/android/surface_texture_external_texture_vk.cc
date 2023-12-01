// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <GLES3/gl3.h>
#include <android/surface_texture.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <memory>

#include "flutter/fml/platform/android/scoped_java_ref.h"
#include "flutter/impeller/renderer/backend/vulkan/context_vk.h"
#include "flutter/shell/platform/android/jni/platform_view_android_jni.h"
#include "flutter/shell/platform/android/surface_texture_external_texture_vk.h"
#include "fml/logging.h"
#include "fml/platform/android/jni_util.h"
#include "fml/platform/android/jni_weak_ref.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/vulkan/android_hardware_buffer_texture_source_vk.h"
#include "impeller/toolkit/egl/config.h"
#include "impeller/toolkit/egl/context.h"
#include "impeller/toolkit/egl/display.h"
#include "impeller/toolkit/egl/surface.h"
#include "shell/platform/android/ndk_helpers.h"
#include "shell/platform/android/surface_texture_external_texture.h"

namespace flutter {

SurfaceTextureExternalTextureVK::SurfaceTextureExternalTextureVK(
    const std::shared_ptr<impeller::ContextVK>& context,
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& surface_texture,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade)
    : SurfaceTextureExternalTexture(id, surface_texture, jni_facade),
      impeller_context_(context) {}

SurfaceTextureExternalTextureVK::~SurfaceTextureExternalTextureVK() {}

void SurfaceTextureExternalTextureVK::ProcessFrame(PaintContext& context,
                                                   const SkRect& bounds) {
  if (state_ == AttachmentState::kUninitialized) {
    // Create an image reader.
    AImageReader* reader = nullptr;
    auto status = NDKHelpers::AImageReader_newWithUsage(
        /*width=*/bounds.width(),
        /*height=*/bounds.height(),
        /*format=*/AIMAGE_FORMAT_RGBA_8888,
        /*usage=*/AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        /*maxImages=*/2,
        /*reader=*/&reader);
    if (status != AMEDIA_OK || !reader) {
      FML_LOG(ERROR) << "Failed to create image reader.";
      return;
    }

    // Add a listener to the image reader.
    auto listener = AImageReader_ImageListener{
        .context = this,
        .onImageAvailable =
            [](void* context, AImageReader* reader) {
              static_cast<SurfaceTextureExternalTextureVK*>(context)
                  ->OnImageAvailable(reader);
            },
    };

    // TODO: Block on this???
    // See acquireLatestImage()
    status = NDKHelpers::AImageReader_setImageListener(reader, &listener);
    if (status != AMEDIA_OK) {
      FML_LOG(ERROR) << "Failed to set image listener.";
      return;
    }

    // Get the surface from the image reader.
    ANativeWindow* window = nullptr;
    status = NDKHelpers::AImageReader_getWindow(reader, &window);
    if (status != AMEDIA_OK || !window) {
      FML_LOG(ERROR) << "Failed to get window from image reader.";
      return;
    }

    // TODO: Create an EGL display once per context.
    egl_ = std::make_unique<impeller::egl::Display>();
    if (!egl_->IsValid()) {
      FML_LOG(ERROR) << "Failed to create EGL display.";
      return;
    }

    // Create a context.
    // These values are basically copied from android_context_gl_impeller.cc.
    auto desc = impeller::egl::ConfigDescriptor{
        .api = impeller::egl::API::kOpenGLES2,
        .samples = impeller::egl::Samples::kOne,
        .color_format = impeller::egl::ColorFormat::kRGBA8888,
        .stencil_bits = impeller::egl::StencilBits::kEight,
        .depth_bits = impeller::egl::DepthBits::kZero,
    };
    auto config = egl_->ChooseConfig(desc);
    if (!config || !config->IsValid()) {
      FML_LOG(ERROR) << "Failed to choose EGL config.";
      return;
    }
    auto context = egl_->CreateContext(*config, nullptr);
    if (!context || !context->IsValid()) {
      FML_LOG(ERROR) << "Failed to create EGL context.";
      return;
    }

    // Create a surface from the image reader.
    surface_ = egl_->CreateWindowSurface(*config, window);
    if (!surface_ || !surface_->IsValid()) {
      FML_LOG(ERROR) << "Failed to create EGL surface.";
      return;
    }

    // "Call to OpenGL ES API with no current context".
    FML_LOG(ERROR) << "Surface: " << surface_->GetHandle();
    if (!context->MakeCurrent(*surface_)) {
      FML_LOG(ERROR) << "Failed to make EGL context current.";
      return;
    }

    // TODO: Create the GLES proc table once.
    gl_ = std::make_unique<impeller::ProcTableGLES>(
        impeller::egl::CreateProcAddressResolver());
    if (!gl_->IsValid()) {
      FML_LOG(ERROR) << "Could not create OpenGL proc table.";
      return;
    }

    // Create a GLES texture and attach it.
    GLuint handle = GL_NONE;
    gl_->GenTextures(1u, &handle);

    // Convert SurfaceTextureWrapper to SurfaceTexture.
    if (surface_texture_.is_null()) {
      FML_LOG(ERROR) << "Surface texture is null.";
      return;
    }

    // This is actually a SurfaceTextureWrapper object.
    // We need to call the underlying ".surfaceTexture()" method.
    JNIEnv* env = fml::jni::AttachCurrentThread();
    fml::jni::ScopedJavaLocalRef<jobject> wrapper(surface_texture_);
    if (wrapper.is_null()) {
      FML_LOG(ERROR) << "Failed to get a reference to surface texture.";
      return;
    }
    auto getWeakReferenceMethod = env->GetMethodID(
        /*clazz=*/env->FindClass("java/lang/ref/WeakReference"),
        /*name=*/"get",
        /*sig=*/"()Ljava/lang/Object;");
    if (!getWeakReferenceMethod) {
      FML_LOG(ERROR) << "Failed to get getWeakReferenceMethod.";
      return;
    }
    auto strong = fml::jni::ScopedJavaLocalRef<jobject>(
        /*env=*/env,
        /*obj=*/env->CallObjectMethod(
            /*obj=*/wrapper.obj(),
            /*methodID=*/getWeakReferenceMethod));
    if (strong.is_null()) {
      FML_LOG(ERROR) << "Failed to get strong reference.";
      return;
    }
    auto surfaceTextureWrapperClass = env->GetObjectClass(strong.obj());
    if (!surfaceTextureWrapperClass) {
      FML_LOG(ERROR) << "Failed to get surfaceTextureWrapperClass.";
      return;
    }
    auto getSurfaceTextureMethod = env->GetMethodID(
        /*clazz=*/surfaceTextureWrapperClass,
        /*name=*/"surfaceTexture",
        /*sig=*/"()Landroid/graphics/SurfaceTexture;");
    if (!getSurfaceTextureMethod) {
      FML_LOG(ERROR) << "Failed to get surfaceTexture method.";
      return;
    }
    auto ref = fml::jni::ScopedJavaLocalRef<jobject>(
        /*env=*/env,
        /*obj=*/env->CallObjectMethod(
            /*obj=*/strong.obj(),
            /*methodID=*/getSurfaceTextureMethod));
    if (ref.is_null()) {
      FML_LOG(ERROR) << "Failed to get surface texture.";
      return;
    }

    ASurfaceTexture* surface_texture =
        NDKHelpers::ASurfaceTexture_fromSurfaceTexture(
            /*env=*/env,
            /*surfaceTextureObj=*/ref.obj());
    if (!surface_texture) {
      FML_LOG(ERROR) << "Failed to get ASurfaceTexture.";
      return;
    }

    // ERROR: attachToContext: SurfaceTexture is already attached to a context
    NDKHelpers::ASurfaceTexture_detachFromGLContext(surface_texture);
    NDKHelpers::ASurfaceTexture_attachToGLContext(surface_texture, handle);
    // Read framebuffer
    // bind draw framebuffer
    // blit
    // done?

    Attach(Id());
  }

  // Blit the image from the image reader to the surface.

  surface_->Present();

  // TODO: Take the hardware buffer from the ImageReader annd render as DlImage.

  if (state_ == AttachmentState::kUninitialized) {
    state_ = AttachmentState::kAttached;
  }
}

void SurfaceTextureExternalTextureVK::OnImageAvailable(AImageReader* reader) {
  // TODO: I think I need to discard the previous image/buffer.

  FML_LOG(ERROR) << "::OnImageAvailable";

  // Get the image from the image reader.
  AImage* image = nullptr;
  auto status = NDKHelpers::AImageReader_acquireLatestImage(reader, &image);
  if (status != AMEDIA_OK || !image) {
    FML_LOG(ERROR) << "Failed to acquire next image.";
    return;
  }

  // Get the hardware buffer from the image.
  AHardwareBuffer* buffer = nullptr;
  status = NDKHelpers::AImage_getHardwareBuffer(image, &buffer);
  if (status != AMEDIA_OK || !buffer) {
    FML_LOG(ERROR) << "Failed to get hardware buffer.";
    return;
  }

  AHardwareBuffer_Desc hb_desc = {};
  flutter::NDKHelpers::AHardwareBuffer_describe(buffer, &hb_desc);
  // hb_desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

  impeller::TextureDescriptor desc;
  desc.storage_mode = impeller::StorageMode::kDevicePrivate;
  desc.size = {static_cast<int>(hb_desc.width),
               static_cast<int>(hb_desc.height)};
  desc.format = impeller::PixelFormat::kR8G8B8A8UNormInt;
  desc.mip_count = 1;
  desc.usage = static_cast<impeller::TextureUsageMask>(
      impeller::TextureUsage::kShaderRead);
  auto source =
      std::make_shared<impeller::AndroidHardwareBufferTextureSourceVK>(
          desc, impeller_context_->GetDevice(), buffer, hb_desc);
  texture_ = std::make_shared<impeller::TextureVK>(impeller_context_, source);
  dl_image_ = impeller::DlImageImpeller::Make(texture_);
}

void SurfaceTextureExternalTextureVK::Detach() {
  SurfaceTextureExternalTexture::Detach();
}

}  // namespace flutter
