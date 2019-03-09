// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.components.embedder_support.view;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.WindowAndroid;

/***
 * This view is used by a ContentView to render its content.
 * Call {@link #setCurrentWebContents(WebContents)} with the webContents that should be
 * managing the content.
 * Note that only one WebContents can be shown at a time.
 */
@JNINamespace("embedder_support")
public class ContentViewRenderView extends FrameLayout {
    // The native side of this object.
    private long mNativeContentViewRenderView;
    private SurfaceHolder.Callback mSurfaceCallback;
    private WindowAndroid mWindowAndroid;

    private final SurfaceView mSurfaceView;
    protected WebContents mWebContents;

    private int mWidth;
    private int mHeight;

    // Enum for the type of compositing surface:
    //   SURFACE_VIEW - Use SurfaceView as compositing surface which
    //                  has a bit performance advantage
    //   TEXTURE_VIEW - Use TextureView as compositing surface which
    //                  supports animation on the View
    public enum CompositingSurfaceType { SURFACE_VIEW, TEXTURE_VIEW };

    // The stuff for TextureView usage. It is not a good practice to mix 2 different
    // implementations into one single class. However, for the sake of reducing the
    // effort of rebasing maintanence in future, here we avoid heavily changes in
    // this class.
    private TextureView mTextureView;
    private Surface mSurface;
    private CompositingSurfaceType mCompositingSurfaceType;

    // The listener which will be triggered when below two conditions become valid.
    // 1. The view has been initialized and ready to draw content to the screen.
    // 2. The compositor finished compositing and the OpenGL buffers has been swapped.
    //    Which means the view has been updated with visually non-empty content.
    // This listener will be triggered only once after registered.
    private FirstRenderedFrameListener mFirstRenderedFrameListener;
    private boolean mFirstFrameReceived;

    public interface FirstRenderedFrameListener{
        public void onFirstFrameReceived();
    }

    // Initialize the TextureView for rendering ContentView and configure the callback
    // listeners.
    private void initTextureView(Context context) {
        mTextureView = new TextureView(context);
        mTextureView.setBackgroundColor(Color.WHITE);

        mTextureView.setSurfaceTextureListener(new SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture,
                    int width, int height) {
                assert mNativeContentViewRenderView != 0;

                mSurface = new Surface(surfaceTexture);
                nativeSurfaceCreated(mNativeContentViewRenderView);
                // Force to trigger the compositor to start working.
                onSurfaceTextureSizeChanged(surfaceTexture, width, height);
                onReadyToRender();
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture,
                    int width, int height) {
                assert mNativeContentViewRenderView != 0 && mSurface != null;
                assert surfaceTexture == mTextureView.getSurfaceTexture();
                assert mSurface != null;

                // Here we hard-code the pixel format since the native part requires
                // the format parameter to decide if the compositing surface should be
                // replaced with a new one when the format is changed.
                //
                // If TextureView is used, the surface won't be possible to changed,
                // so that the format is also not changed. There is no special reason
                // to use RGBA_8888 value since the native part won't use its real
                // value to do something for drawing.
                //
                // TODO(hmin): Figure out how to get pixel format from SurfaceTexture.
                int format = PixelFormat.RGBA_8888;
                nativeSurfaceChanged(mNativeContentViewRenderView,
                        format, width, height, mSurface);
                if (mContentViewCore != null) {
                    mContentViewCore.onPhysicalBackingSizeChanged(
                            width, height);
                }
            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
                assert mNativeContentViewRenderView != 0;
                nativeSurfaceDestroyed(mNativeContentViewRenderView);

                // Release the underlying surface to make it invalid.
                mSurface.release();
                mSurface = null;
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {
                // Do nothing since the SurfaceTexture won't be updated via updateTexImage().
            }
        });
    }

    public ContentViewRenderView(Context context) {
        this(context, CompositingSurfaceType.SURFACE_VIEW);
    }

    /**
     * Constructs a new ContentViewRenderView.
     * This should be called and the {@link ContentViewRenderView} should be added to the view
     * hierarchy before the first draw to avoid a black flash that is seen every time a
     * {@link SurfaceView} is added.
     * @param context The context used to create this.
     * @param surfaceType TextureView is used as compositing target surface,
     *                    otherwise SurfaceView is used.
     */
    public ContentViewRenderView(Context context, CompositingSurfaceType surfaceType) {
        super(context);

        mCompositingSurfaceType = surfaceType;
        if (surfaceType == CompositingSurfaceType.TEXTURE_VIEW) {
            initTextureView(context);

            addView(mTextureView,
                    new FrameLayout.LayoutParams(
                            FrameLayout.LayoutParams.MATCH_PARENT,
                            FrameLayout.LayoutParams.MATCH_PARENT));

            // Avoid compiler warning.
            mSurfaceView = null;
            mSurfaceCallback = null;
            return;
        }

        mSurfaceView = createSurfaceView(getContext());
        mSurfaceView.setZOrderMediaOverlay(true);

        setSurfaceViewBackgroundColor(Color.WHITE);
        addView(mSurfaceView,
                new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
        mSurfaceView.setVisibility(GONE);
    }

    /**
     * Initialization that requires native libraries should be done here.
     * Native code should add/remove the layers to be rendered through the ContentViewLayerRenderer.
     * @param rootWindow The {@link WindowAndroid} this render view should be linked to.
     */
    public void onNativeLibraryLoaded(WindowAndroid rootWindow) {
        assert !mSurfaceView.getHolder().getSurface().isValid()
            : "Surface created before native library loaded.";
        assert rootWindow != null;
        mNativeContentViewRenderView = nativeInit(rootWindow);
        assert mNativeContentViewRenderView != 0;
        mWindowAndroid = rootWindow;
        mSurfaceCallback = new SurfaceHolder.Callback() {
            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                assert mNativeContentViewRenderView != 0;
                nativeSurfaceChanged(
                        mNativeContentViewRenderView, format, width, height, holder.getSurface());
                if (mWebContents != null) {
                    nativeOnPhysicalBackingSizeChanged(
                            mNativeContentViewRenderView, mWebContents, width, height);
                }
            }

            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                assert mNativeContentViewRenderView != 0;
                nativeSurfaceCreated(mNativeContentViewRenderView);

                // On pre-M Android, layers start in the hidden state until a relayout happens.
                // There is a bug that manifests itself when entering overlay mode on pre-M devices,
                // where a relayout never happens. This bug is out of Chromium's control, but can be
                // worked around by forcibly re-setting the visibility of the surface view.
                // Otherwise, the screen stays black, and some tests fail.
                mSurfaceView.setVisibility(mSurfaceView.getVisibility());

                onReadyToRender();
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                assert mNativeContentViewRenderView != 0;
                nativeSurfaceDestroyed(mNativeContentViewRenderView);
            }
        };
        mSurfaceView.getHolder().addCallback(mSurfaceCallback);
        mSurfaceView.setVisibility(VISIBLE);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        mWidth = w;
        mHeight = h;
        if (mWebContents != null) mWebContents.setSize(w, h);
    }

    /**
     * View's method override to notify WindowAndroid about changes in its visibility.
     */
    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);

        if (mWindowAndroid == null) return;

        if (visibility == View.GONE) {
            mWindowAndroid.onVisibilityChanged(false);
        } else if (visibility == View.VISIBLE) {
            mWindowAndroid.onVisibilityChanged(true);
        }
    }

    /**
     * Set the background color of SurfaceView or TextureView.  This method is necessary because the
     * background color of ContentViewRenderView itself is covered by the background of
     * SurfaceView or TextureView.
     * @param color The color of the background.
     */
    public void setSurfaceViewBackgroundColor(int color) {
        if (mSurfaceView != null) {
            mSurfaceView.setBackgroundColor(color);
        } else if (mTextureView != null) {
            mTextureView.setOpaque(isOpaque(color));
            mTextureView.setBackgroundColor(color);
        }
    }

    /**
     * Gets the SurfaceView for this ContentViewRenderView
     */
    public SurfaceView getSurfaceView() {
        return mSurfaceView;
    }

    /**
     * Should be called when the ContentViewRenderView is not needed anymore so its associated
     * native resource can be freed.
     */
    public void destroy() {
        mSurfaceView.getHolder().removeCallback(mSurfaceCallback);
        mWindowAndroid = null;
        nativeDestroy(mNativeContentViewRenderView);
        mNativeContentViewRenderView = 0;
    }

    public void setCurrentWebContents(WebContents webContents) {
        assert mNativeContentViewRenderView != 0;
        mWebContents = webContents;

        if (webContents != null) {
            webContents.setSize(mWidth, mHeight);
            nativeOnPhysicalBackingSizeChanged(
                    mNativeContentViewRenderView, webContents, mWidth, mHeight);
        }
        nativeSetCurrentWebContents(mNativeContentViewRenderView, webContents);
    }

    /**
     * This method should be subclassed to provide actions to be performed once the view is ready to
     * render.
     */
    protected void onReadyToRender() {}

    /**
     * This method could be subclassed optionally to provide a custom SurfaceView object to
     * this ContentViewRenderView.
     * @param context The context used to create the SurfaceView object.
     * @return The created SurfaceView object.
     */
    protected SurfaceView createSurfaceView(Context context) {
        return new SurfaceView(context);
    }

    public void registerFirstRenderedFrameListener(FirstRenderedFrameListener listener) {
        mFirstRenderedFrameListener = listener;
        if (mFirstFrameReceived && mFirstRenderedFrameListener != null) {
            mFirstRenderedFrameListener.onFirstFrameReceived();
        }
    }

    /**
     * @return whether the surface view is initialized and ready to render.
     */
    public boolean isInitialized() {
        return mSurfaceView.getHolder().getSurface() != null || mSurface != null;
    }

    /**
    * Control whether the SurfaceView's surface is placed on top of its window.
    * Note this only works when SurfaceView is used. For TextureView, it is not supported.
    * @param onTop true for on top.
    */
    public void setZOrderOnTop(boolean onTop) {
        if (mSurfaceView == null) return;
        mSurfaceView.setZOrderOnTop(onTop);
    }

    /**
     * Enter or leave overlay video mode.
     * @param enabled Whether overlay mode is enabled.
     */
    public void setOverlayVideoMode(boolean enabled) {
        if (mCompositingSurfaceType == CompositingSurfaceType.TEXTURE_VIEW) {
            nativeSetOverlayVideoMode(mNativeContentViewRenderView, enabled);
            return;
        }
        int format = enabled ? PixelFormat.TRANSLUCENT : PixelFormat.OPAQUE;
        mSurfaceView.getHolder().setFormat(format);
        nativeSetOverlayVideoMode(mNativeContentViewRenderView, enabled);
    }

    @CalledByNative
    private void didSwapFrame() {
        if (mSurfaceView.getBackground() != null) {
            post(new Runnable() {
                @Override
                public void run() {
                    mSurfaceView.setBackgroundResource(0);
                }
            });
        }
    }

    private native long nativeInit(WindowAndroid rootWindow);
    private native void nativeDestroy(long nativeContentViewRenderView);
    private native void nativeSetCurrentWebContents(
            long nativeContentViewRenderView, WebContents webContents);
    private native void nativeOnPhysicalBackingSizeChanged(
            long nativeContentViewRenderView, WebContents webContents, int width, int height);
    private native void nativeSurfaceCreated(long nativeContentViewRenderView);
    private native void nativeSurfaceDestroyed(long nativeContentViewRenderView);
    private native void nativeSurfaceChanged(
            long nativeContentViewRenderView, int format, int width, int height, Surface surface);
    private native void nativeSetOverlayVideoMode(
            long nativeContentViewRenderView, boolean enabled);
}
