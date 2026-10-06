package com.lvonasek.arcore3dscanner.ui;

import androidx.appcompat.app.AlertDialog;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.util.Log;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.lvonasek.arcore3dscanner.main.Exporter;
import com.lvonasek.arcore3dscanner.R;

import java.io.File;

public class RenameDialog {

    private EditText mInput;
    private TextView mPath;

    public RenameDialog(FileManager context, String path, String key) {
        AlertDialog.Builder renameDlg = new MaterialAlertDialogBuilder(context);
        renameDlg.setView(R.layout.dialog_rename);
        renameDlg.setPositiveButton(android.R.string.ok, (dialog, which) -> {
            int type = Exporter.getModelType(key);
            String name1 = mInput.getText().toString();
            if (type >= 0) {
                name1 = name1 + Exporter.FILE_EXT[type];
            }
            File newFile = new File(mPath.getText().toString(), name1);
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

        AlertDialog d = renameDlg.create();
        d.show();

        String name = key;
        if (name.contains(".")) {
            name = name.substring(0, name.lastIndexOf('.'));
        }
        mInput = d.findViewById(R.id.filename);
        mInput.setText(name);
        mPath = d.findViewById(R.id.path);
        mPath.setText(path);

        FolderAdapter adapter = new FolderAdapter(context, path, mPath::setText);
        ((ListView) d.findViewById(R.id.list)).setAdapter(adapter);
        adapter.update();
    }
}
