/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositorSWGL.h"

#include "mozilla/gfx/Logging.h"
#include "mozilla/widget/CompositorWidget.h"

namespace mozilla {
namespace wr {

/* static */
UniquePtr<RenderCompositor> RenderCompositorSWGL::Create(
    RefPtr<widget::CompositorWidget>&& aWidget, nsACString& aError) {
  void* ctx = wr_swgl_create_context();
  if (!ctx) {
    gfxCriticalNote << "Failed SWGL context creation for WebRender";
    return nullptr;
  }
  return MakeUnique<RenderCompositorSWGL>(std::move(aWidget), ctx);
}

RenderCompositorSWGL::RenderCompositorSWGL(
    RefPtr<widget::CompositorWidget>&& aWidget, void* aContext)
    : RenderCompositor(std::move(aWidget)), mContext(aContext) {
  MOZ_ASSERT(mContext);
}

RenderCompositorSWGL::~RenderCompositorSWGL() {
  wr_swgl_destroy_context(mContext);
}

void RenderCompositorSWGL::ClearMappedBuffer() {
  mMappedData = nullptr;
  mMappedStride = 0;
  mDT = nullptr;
}

bool RenderCompositorSWGL::MakeCurrent() {
  wr_swgl_make_current(mContext);
  return true;
}

bool RenderCompositorSWGL::BeginFrame() {
  // Set up a temporary region representing the entire window surface in case a
  // dirty region is not supplied.
  ClearMappedBuffer();
  mRegion = LayoutDeviceIntRect(LayoutDeviceIntPoint(), GetBufferSize());
  wr_swgl_make_current(mContext);
  return true;
}

bool RenderCompositorSWGL::AllocateMappedBuffer() {
  // Request a new draw target to use from the widget...
  MOZ_ASSERT(!mDT);
  layers::BufferMode bufferMode = layers::BufferMode::BUFFERED;
  mDT = mWidget->StartRemoteDrawingInRegion(mRegion, &bufferMode);
  if (!mDT) {
    return false;
  }
  // Attempt to lock the underlying buffer directly from the draw target.
  // Verify that the size at least matches what the widget claims and that
  // the format is BGRA8 as SWGL requires.
  uint8_t* data = nullptr;
  gfx::IntSize size;
  int32_t stride = 0;
  gfx::SurfaceFormat format = gfx::SurfaceFormat::UNKNOWN;
  if (!mSurface && mDT->LockBits(&data, &size, &stride, &format) &&
      (format != gfx::SurfaceFormat::B8G8R8A8 &&
       format != gfx::SurfaceFormat::B8G8R8X8)) {
    // We tried to lock the DT and it succeeded, but the size or format
    // of the data is not compatible, so just release it and fall back below...
    mDT->ReleaseBits(data);
    data = nullptr;
  }
  LayoutDeviceIntRect bounds = mRegion.GetBounds();
  // If locking succeeded above, just use that.
  if (data) {
    mMappedData = data;
    mMappedStride = stride;
    // Disambiguate whether the widget's draw target has its origin at zero or
    // if it is offset to the dirty region origin.
    if (size == gfx::IntSize(bounds.XMost(), bounds.YMost())) {
      // Update the bounds to include zero if the origin is at zero.
      bounds.ExpandToEnclose(LayoutDeviceIntPoint(0, 0));
    }
  } else {
    // If we couldn't lock the DT above, then allocate a data surface and map
    // that for usage with SWGL.
    size = bounds.Size().ToUnknownSize();
    if (!mSurface || mSurface->GetSize() != size) {
      mSurface = gfx::Factory::CreateDataSourceSurface(
          size, gfx::SurfaceFormat::B8G8R8A8);
    }
    gfx::DataSourceSurface::MappedSurface map = {nullptr, 0};
    if (!mSurface || !mSurface->Map(gfx::DataSourceSurface::READ_WRITE, &map)) {
      // We failed mapping the data surface, so need to cancel the frame.
      mWidget->EndRemoteDrawingInRegion(mDT, mRegion);
      ClearMappedBuffer();
      return false;
    }
    mMappedData = map.mData;
    mMappedStride = map.mStride;
  }
  MOZ_ASSERT(mMappedData != nullptr && mMappedStride > 0);
  wr_swgl_init_default_framebuffer(mContext, bounds.x, bounds.y, bounds.width,
                                   bounds.height, mMappedStride, mMappedData);
  return true;
}

void RenderCompositorSWGL::StartCompositing(
    const wr::DeviceIntRect* aDirtyRects, size_t aNumDirtyRects) {
  if (mDT) {
    // Cancel any existing buffers that might accidentally be left from updates
    CommitMappedBuffer(false);
    // Reset the region to the widget bounds
    mRegion = LayoutDeviceIntRect(LayoutDeviceIntPoint(), GetBufferSize());
  }
  if (aNumDirtyRects) {
    // Install the dirty rects into the bounds of the existing region
    auto bounds = mRegion.GetBounds();
    mRegion.SetEmpty();
    for (size_t i = 0; i < aNumDirtyRects; i++) {
      const auto& rect = aDirtyRects[i];
      mRegion.OrWith(LayoutDeviceIntRect(rect.origin.x, rect.origin.y,
                                         rect.size.width, rect.size.height));
    }
    // Ensure the region lies within the widget bounds
    mRegion.AndWith(bounds);
  }
  // Now that the dirty rects have been supplied and the composition region
  // is known, allocate and install a framebuffer encompassing the composition
  // region.
  if (!AllocateMappedBuffer()) {
    gfxCriticalNote
        << "RenderCompositorSWGL failed mapping default framebuffer";
    // If allocation of the mapped default framebuffer failed, then just install
    // a small temporary framebuffer so compositing can still proceed.
    wr_swgl_init_default_framebuffer(mContext, 0, 0, 2, 2, 0, nullptr);
  }
}

void RenderCompositorSWGL::CommitMappedBuffer(bool aDirty) {
  if (!mDT) {
    return;
  }
  // If we have a draw target at this point, mapping must have succeeded.
  MOZ_ASSERT(mMappedData != nullptr);
  if (mSurface) {
    // If we're using a data surface, unmap it and draw it to the DT if there
    // are any supplied dirty rects.
    mSurface->Unmap();
    if (aDirty) {
      for (auto iter = mRegion.RectIter(); !iter.Done(); iter.Next()) {
        const LayoutDeviceIntRect& dirtyRect = iter.Get();
        gfx::Rect bounds(dirtyRect.x, dirtyRect.y, dirtyRect.width,
                         dirtyRect.height);
        mDT->DrawSurface(mSurface, bounds, bounds,
                         gfx::DrawSurfaceOptions(gfx::SamplingFilter::POINT),
                         gfx::DrawOptions(1.0f, gfx::CompositionOp::OP_SOURCE));
      }
    }
  } else {
    // Otherwise, we had locked the DT directly. Just release the data.
    mDT->ReleaseBits(mMappedData);
  }
  // Done with the DT. Hand it back to the widget and clear out any trace of it.
  mWidget->EndRemoteDrawingInRegion(mDT, mRegion);
  ClearMappedBuffer();
}

void RenderCompositorSWGL::CancelFrame() { CommitMappedBuffer(false); }

RenderedFrameId RenderCompositorSWGL::EndFrame(
    const nsTArray<DeviceIntRect>& aDirtyRects) {
  // Dirty rects have already been set inside StartCompositing. We need to keep
  // those dirty rects exactly the same here so we supply the same exact region
  // to EndRemoteDrawingInRegion as for StartRemoteDrawingInRegion.
  RenderedFrameId frameId = GetNextRenderFrameId();
  CommitMappedBuffer();
  return frameId;
}

void RenderCompositorSWGL::Pause() {}

bool RenderCompositorSWGL::Resume() { return true; }

LayoutDeviceIntSize RenderCompositorSWGL::GetBufferSize() {
  return mWidget->GetClientSize();
}

CompositorCapabilities RenderCompositorSWGL::GetCompositorCapabilities() {
  CompositorCapabilities caps;

  // don't use virtual surfaces
  caps.virtual_surface_size = 0;

  return caps;
}

}  // namespace wr
}  // namespace mozilla
