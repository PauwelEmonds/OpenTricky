// OpenTrickyActivity - SDL's activity plus what the game needs from Android: the disc image, chosen once in the system's
// file picker and remembered (a persistable permission on its content URI), handed to the game as a file descriptor it
// keeps open and reads the disc from; and dialogs the game can block on.
// Testing: am start -n io.github.opentricky.ssxtricky/io.github.opentricky.OpenTrickyActivity
//              -e args "--play" -e env "XBOX_FPS_LOG=1;XBOX_INPUT_AUTOPRESS=start@6"
package io.github.opentricky;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

public class OpenTrickyActivity extends SDLActivity {
    private static final String TAG = "OpenTricky";
    private static final int REQ_ISO = 0x0715;
    private static final String PREFS = "opentricky", KEY_ISO = "disc_image_uri";

    private static final Object sLock = new Object();
    private static boolean sPicked;
    private static Uri sPick;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {                  // testing: the command line of port/src/main.c
        String a = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return a == null || a.trim().isEmpty() ? new String[0] : a.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle state) {
        String env = getIntent() != null ? getIntent().getStringExtra("env") : null;   // testing: XBOX_* settings, "K=V;K=V"
        if (env != null) for (String kv : env.split(";")) {
            int i = kv.indexOf('=');
            if (i > 0) try { Os.setenv(kv.substring(0, i).trim(), kv.substring(i + 1), true); } catch (Exception e) { Log.w(TAG, "setenv " + kv, e); }
        }
        super.onCreate(state);
    }

    // ---- called from the game's thread (port/src/host_sdl.c) ---------------------------------------------------------

    /** The disc image as a file descriptor the game keeps open: the one chosen before if it still opens (forget = false),
     *  else the system's file picker. -1 = cancelled, -2 = failed. Blocks until the player is done. */
    public static int openDiscImage(boolean forget) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return -2;
        SharedPreferences p = a.getSharedPreferences(PREFS, 0);
        String saved = forget ? null : p.getString(KEY_ISO, null);
        if (saved != null) {
            int fd = openFd(a, Uri.parse(saved));
            if (fd >= 0) return fd;
            Log.w(TAG, "the disc image chosen before no longer opens: " + saved);
        }
        synchronized (sLock) { sPicked = false; sPick = null; }
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_TITLE, "Your SSX Tricky (USA) Xbox disc image (.iso)");
            try { a.startActivityForResult(i, REQ_ISO); }
            catch (Exception e) { Log.e(TAG, "no file picker", e); picked(null); }
        });
        Uri u;
        synchronized (sLock) {
            while (!sPicked) try { sLock.wait(); } catch (InterruptedException e) { return -1; }
            u = sPick;
        }
        if (u == null) return -1;
        try {
            a.getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            Log.w(TAG, "no persistable permission for " + u + " (it will be asked for again)", e);
        }
        p.edit().putString(KEY_ISO, u.toString()).apply();
        int fd = openFd(a, u);
        return fd >= 0 ? fd : -2;
    }

    private static int openFd(Activity a, Uri u) {
        try {
            ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(u, "r");
            return pfd != null ? pfd.detachFd() : -2;
        } catch (Exception e) {
            return -2;
        }
    }

    private static void picked(Uri u) {
        synchronized (sLock) { sPick = u; sPicked = true; sLock.notifyAll(); }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == REQ_ISO) { picked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        super.onActivityResult(request, result, data);
    }

    /** A message with up to three buttons over the game; 1 for b1, 0 for b2, -1 for b3. Blocks until one is pressed. */
    public static int dialog(final String text, final String b1, final String b2, final String b3) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return -1;
        final Object lock = new Object();
        final int[] r = { -2 };
        a.runOnUiThread(() -> {
            AlertDialog.Builder d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("SSX Tricky").setMessage(text).setCancelable(false);
            if (b1 != null) d.setPositiveButton(b1, (x, w) -> answer(lock, r, 1));
            if (b2 != null) d.setNeutralButton(b2, (x, w) -> answer(lock, r, 0));
            if (b3 != null) d.setNegativeButton(b3, (x, w) -> answer(lock, r, -1));
            d.show();
        });
        synchronized (lock) {
            while (r[0] == -2) try { lock.wait(); } catch (InterruptedException e) { return -1; }
        }
        return r[0];
    }

    private static void answer(Object lock, int[] r, int v) {
        synchronized (lock) { r[0] = v; lock.notifyAll(); }
    }
}
