package com.zhixiaoban.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * 极简文件共享 Provider：只用于把"拍照"得到的临时图片交给系统相机去写。
 * 不走 AndroidX，所以自己实现，仅暴露应用缓存目录下的 shared/ 目录。
 */
public class SimpleFileProvider extends ContentProvider {

    public static final String AUTH_SUFFIX = ".files";

    public static File sharedDir(Context c) {
        File d = new File(c.getCacheDir(), "shared");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    public static Uri uriFor(Context c, File f) {
        return new Uri.Builder()
                .scheme("content")
                .authority(c.getPackageName() + AUTH_SUFFIX)
                .appendPath(f.getName())
                .build();
    }

    private File resolve(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || name.contains("..")) return null;
        return new File(sharedDir(getContext()), name);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = resolve(uri);
        if (f == null) throw new FileNotFoundException("bad uri");
        return ParcelFileDescriptor.open(f,
                ParcelFileDescriptor.MODE_READ_WRITE | ParcelFileDescriptor.MODE_CREATE);
    }

    @Override
    public String getType(Uri uri) {
        String n = uri.getLastPathSegment();
        String ext = "";
        if (n != null && n.lastIndexOf('.') >= 0) ext = n.substring(n.lastIndexOf('.') + 1).toLowerCase();
        String t = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        return t == null ? "image/jpeg" : t;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File f = resolve(uri);
        String[] cols = (projection != null)
                ? projection
                : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = uri.getLastPathSegment();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = (f == null ? 0L : f.length());
            else row[i] = null;
        }
        c.addRow(row);
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
