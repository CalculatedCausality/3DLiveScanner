package com.lvonasek.arcore3dscanner.ui;

import androidx.appcompat.app.AlertDialog;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.R;

import java.io.File;

public class RenameDialog {
    public RenameDialog(FileManager context, String path, String key) {
        AlertDialog.Builder renameDlg = new MaterialAlertDialogBuilder(context);
        View content = LayoutInflater.from(renameDlg.getContext()).inflate(R.layout.dialog_rename, null);
        EditText input = content.findViewById(R.id.filename);
        TextView targetPath = content.findViewById(R.id.path);
        input.setText(key.contains(".") ? key.substring(0, key.lastIndexOf('.')) : key);
        targetPath.setText(path);
        renameDlg.setView(content);
        renameDlg.setPositiveButton(android.R.string.ok, (dialog, which) -> {
            int type = Exporter.getModelType(key);
            String name1 = input.getText().toString();
            if (type >= 0) {
                name1 = name1 + Exporter.FILE_EXT[type];
            }
            File newFile = new File(targetPath.getText().toString(), name1);
            if(newFile.exists())
                Toast.makeText(context, R.string.name_exists, Toast.LENGTH_LONG).show();
            else {
                File oldFile = new File(path, key);
                if (oldFile.renameTo(newFile))
                    Log.d(AbstractActivity.TAG, "File " + oldFile + " renamed to " + newFile);
                context.refreshUI();
            }
        });
        renameDlg.setNegativeButton(android.R.string.cancel, null);

        FolderAdapter adapter = new FolderAdapter(context, path, targetPath::setText);
        ((ListView) content.findViewById(R.id.list)).setAdapter(adapter);
        adapter.update();
        renameDlg.show();
    }
}
