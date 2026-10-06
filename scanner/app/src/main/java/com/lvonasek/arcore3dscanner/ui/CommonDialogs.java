package com.lvonasek.arcore3dscanner.ui;

import android.app.Activity;
import android.app.Dialog;
import androidx.appcompat.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.GridView;
import android.widget.TextView;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.lvonasek.arcore3dscanner.R;
import java.util.List;

public class CommonDialogs {

    static Dialog showScanChoices(Context context, List<String> values, List<Drawable> icons, int title) {
        Dialog dialog = new MaterialAlertDialogBuilder(context)
                .setView(R.layout.dialog_scan)
                .setNegativeButton(android.R.string.cancel, null)
                .create();
        dialog.show();
        if (title != 0) ((TextView) dialog.findViewById(R.id.name)).setText(title);
        ((GridView) dialog.findViewById(R.id.list)).setAdapter(new ArrayAdapterWithIcons(context, values, icons));
        return dialog;
    }

    public static void confirmDialog(Activity context, int title, Runnable proceed) {
        AlertDialog.Builder dialog = new MaterialAlertDialogBuilder(context);
        boolean discard = title == R.string.scan_discard;
        boolean finish = title == R.string.scan_finish;
        dialog.setTitle(discard ? R.string.action_discard_scan
                : (finish ? R.string.action_finish_scan : title));
        dialog.setMessage(discard ? R.string.scan_discard_message
                : (finish ? R.string.scan_finish_message : R.string.continue_question));
    dialog.setPositiveButton(discard ? R.string.scan_discard_short
                : (finish ? R.string.scan_finish_short
                : (title == R.string.delete ? R.string.delete : android.R.string.ok)), (d, which) -> proceed.run());
        dialog.setNegativeButton(android.R.string.cancel, null);
        AlertDialog d = dialog.create();
        d.setOnDismissListener(dialogInterface -> setImmersive(context.getWindow()));

        //workaround to system UI glitch
        setImmersive(d, context);
        d.show();
    }

    public static void setImmersive(AlertDialog d, Activity context) {
        d.getWindow().setFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        d.getWindow().getDecorView().setSystemUiVisibility(context.getWindow().getDecorView().getSystemUiVisibility());
        d.setOnShowListener(dialog1 -> {
            d.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            wm.updateViewLayout(d.getWindow().getDecorView(), d.getWindow().getAttributes());
        });
    }

    public static void setImmersive(Window window) {
        window.peekDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
