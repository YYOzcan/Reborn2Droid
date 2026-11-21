package org.reborn64;

import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    private static GameActivity self;
    private TextView overlay;

    @Override protected String[] getLibraries() { return new String[] { "SDL2", "main" }; }

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        self = this;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= 28)
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        overlay = new TextView(this);
        overlay.setTextColor(Color.YELLOW);
        overlay.setShadowLayer(3, 1, 1, Color.BLACK);
        overlay.setTextSize(14);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.RIGHT);
        addContentView(overlay, lp);
        hideSystemUi();
    }

    /** called from native code with a short status line (FPS) */
    public static void setOverlay(final String text) {
        final GameActivity a = self;
        if (a == null) return;
        a.runOnUiThread(new Runnable() { public void run() { if (a.overlay != null) a.overlay.setText(text); } });
    }

    /** Unity's Screen.orientation (ActivityInfo.SCREEN_ORIENTATION_* values) */
    public static void requestOrientation(final int o) {
        final GameActivity a = self;
        if (a == null) return;
        a.runOnUiThread(new Runnable() { public void run() { a.setRequestedOrientation(o); } });
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
}
