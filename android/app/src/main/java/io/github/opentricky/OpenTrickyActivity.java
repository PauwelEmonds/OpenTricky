// OpenTrickyActivity - SDL's activity plus what the game needs from Android: the disc image, chosen once in the system's
// file picker and remembered (a persistable permission on its content URI), handed to the game as a file descriptor it
// keeps open and reads the disc from; and dialogs the game can block on.
// An APK built with the image inside (android/pack_iso.bat: assets/game.iso, stored uncompressed) uses that one instead:
// the descriptor is the APK's own, the image at discImageOffset() in it.
// Testing: am start -n io.github.opentricky.ssxtricky/io.github.opentricky.OpenTrickyActivity
//              -e args "--play" -e env "XBOX_FPS_LOG=1;XBOX_INPUT_AUTOPRESS=start@6"
package io.github.opentricky;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.util.Log;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.Switch;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

public class OpenTrickyActivity extends SDLActivity {
    private static final String TAG = "OpenTricky";
    private static final int REQ_ISO = 0x0715;
    private static final String PREFS = "opentricky", KEY_ISO = "disc_image_uri";
    private static final String PACKED_ISO = "game.iso";            // android/pack_iso.bat

    private static volatile long sOffset;                           // of the image in the file openDiscImage returned

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
        sOffset = 0;
        if (!forget) {                                  // the image packed into the APK, if there is one
            try (AssetFileDescriptor afd = a.getAssets().openFd(PACKED_ISO)) {
                int fd = afd.getParcelFileDescriptor().dup().detachFd();
                sOffset = afd.getStartOffset();
                Log.i(TAG, "disc image packed in the APK: " + afd.getLength() + " bytes at " + sOffset);
                return fd;
            } catch (java.io.FileNotFoundException e) {
                // none: the one chosen before, or the picker
            } catch (Exception e) {
                Log.w(TAG, "the disc image packed in the APK does not open", e);
            }
        }
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

    /** Where the disc image starts in the file of the last openDiscImage: 0, or its place in the APK. */
    public static long discImageOffset() {
        return sOffset;
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

    /** The options (the touch controls' options button, or Back): the frame rate, 60 or the screen's rate (hz), and the
     *  frame rate counter. Blocks until closed: -1 cancelled, else the frame rate | 0x10000 with the counter on. */
    public static int options(final int fps, final int hz, final boolean counter) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return -1;
        final Object lock = new Object();
        final int[] r = { -2 };
        a.runOnUiThread(() -> {
            // The views in the dialog's own theme (the activity's would draw the switch as a bare "on/off" label).
            final android.content.Context t = new android.view.ContextThemeWrapper(a, android.R.style.Theme_DeviceDefault_Dialog_Alert);
            final float dp = a.getResources().getDisplayMetrics().density;
            final int pad = (int) (24 * dp);
            LinearLayout box = new LinearLayout(t);
            box.setOrientation(LinearLayout.VERTICAL);
            box.setPadding(pad, (int) (8 * dp), pad, 0);

            TextView head = new TextView(t);
            head.setText("Frame rate");
            head.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            head.setAlpha(0.7f);
            box.addView(head);

            final RadioGroup rate = new RadioGroup(t);
            RadioButton r60 = new RadioButton(t);
            r60.setId(60);
            r60.setText("60 fps  –  as on the Xbox, uses less battery");
            rate.addView(r60);
            if (hz > 61) {
                RadioButton rhz = new RadioButton(t);
                rhz.setId(hz);
                rhz.setText(hz + " fps  –  smoother, the screen's own rate");
                rate.addView(rhz);
            }
            rate.check(fps > 61 && hz > 61 ? hz : 60);
            box.addView(rate);

            final Switch show = new Switch(t);
            show.setText("Show the frame rate");
            show.setChecked(counter);
            show.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
            show.setPadding(0, (int) (16 * dp), 0, (int) (8 * dp));
            box.addView(show);

            AlertDialog d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Graphics")
                .setView(box)
                .setPositiveButton("Done", (x, w) -> answer(lock, r, rate.getCheckedRadioButtonId() | (show.isChecked() ? 0x10000 : 0)))
                .setNegativeButton("Cancel", (x, w) -> answer(lock, r, -1))
                .setOnCancelListener(x -> answer(lock, r, -1))
                .create();
            d.show();
        });
        synchronized (lock) {
            while (r[0] == -2) try { lock.wait(); } catch (InterruptedException e) { return -1; }
        }
        return r[0];
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
