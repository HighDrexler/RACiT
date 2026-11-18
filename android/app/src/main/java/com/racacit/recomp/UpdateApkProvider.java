package com.racacit.recomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.*;

/** Read-only, explicitly granted access to immutable APKs in this app's private update cache. */
public final class UpdateApkProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    private File file(Uri uri) throws FileNotFoundException {
        if (getContext() == null || !"content".equals(uri.getScheme()) ||
                !(getContext().getPackageName() + ".updates").equals(uri.getAuthority()) ||
                uri.getQuery() != null || uri.getFragment() != null || uri.getPathSegments().size() != 1 ||
                !uri.getLastPathSegment().matches("[a-f0-9]{32}\\.apk")) throw new FileNotFoundException("Unknown update document.");
        if (Binder.getCallingUid() != android.os.Process.myUid() && getContext().checkUriPermission(uri,
                Binder.getCallingPid(), Binder.getCallingUid(), Intent.FLAG_GRANT_READ_URI_PERMISSION) != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("A read grant is required.");
        File directory = OfflineUpdate.directory(getContext()), target = new File(directory, uri.getLastPathSegment());
        try {
            if (!target.isFile() || !target.getCanonicalFile().getParentFile().equals(directory.getCanonicalFile()))
                throw new FileNotFoundException("Update document is unavailable.");
        } catch (IOException error) { throw new FileNotFoundException("Update document is unavailable."); }
        return target;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("Updates are read-only.");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public String getType(Uri uri) { return "application/vnd.android.package-archive"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String order) {
        try {
            File target = file(uri);
            String[] columns = projection == null ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
            MatrixCursor cursor = new MatrixCursor(columns, 1);
            Object[] values = new Object[columns.length];
            for (int i = 0; i < columns.length; i++) {
                if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) values[i] = "RACiT-Android-update.apk";
                else if (OpenableColumns.SIZE.equals(columns[i])) values[i] = target.length();
            }
            cursor.addRow(values); return cursor;
        } catch (FileNotFoundException error) { throw new IllegalArgumentException(error.getMessage()); }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("Read-only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException("Read-only"); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException("Read-only"); }
}
