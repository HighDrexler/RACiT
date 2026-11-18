package com.racacit.recomp;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import java.io.*;
import java.security.MessageDigest;
import java.util.*;

/** Stages a local document privately. Android performs final payload/signature verification on install. */
final class OfflineUpdate {
    static final long MAX_BYTES = 512L << 20;
    static final long MAX_CACHE_BYTES = 1L << 30;
    static final class Candidate {
        final File file; final String version; final long code; final Uri uri;
        Candidate(File file, String version, long code, Uri uri) { this.file = file; this.version = version; this.code = code; this.uri = uri; }
    }
    static File directory(Context context) { return new File(context.getCacheDir(), "verified-updates"); }

    static Candidate stage(Context context, InputStream source) throws Exception {
        if (source == null) throw new IOException("The selected APK cannot be opened.");
        File directory = directory(context);
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create APK staging folder.");
        long cached = 0;
        File[] files = directory.listFiles();
        if (files != null) for (File old : files) {
            if (!old.getName().matches("[a-f0-9]{32}\\.apk")) continue;
            if (System.currentTimeMillis() - old.lastModified() > 7L * 24 * 60 * 60 * 1000) old.delete();
            if (old.exists()) cached += old.length();
        }
        // Recent files may still be open in the system installer; never replace or delete them here.
        if (cached >= MAX_CACHE_BYTES) throw new IOException("Update cache is full. Clear this app's cache in Android settings, then select the APK again. Do not clear app storage.");
        File target = new File(directory, UUID.randomUUID().toString().replace("-", "") + ".apk");
        boolean accepted = false;
        try {
            long limit = Math.min(MAX_BYTES, Math.min(MAX_CACHE_BYTES - cached, Math.max(0, directory.getUsableSpace() - (64L << 20))));
            try (FileOutputStream output = new FileOutputStream(target)) {
                byte[] buffer = new byte[65536]; long total = 0; int n;
                while ((n = source.read(buffer)) != -1) {
                    if (n == 0) continue;
                    total += n;
                    if (total > limit) throw new IOException("APK exceeds the 512 MB limit or available staging space.");
                    output.write(buffer, 0, n);
                }
                output.getFD().sync();
            }
            if (!target.setReadOnly()) throw new IOException("Cannot protect the staged APK.");
            PackageManager manager = context.getPackageManager();
            int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
            PackageInfo installed = manager.getPackageInfo(context.getPackageName(), flags);
            PackageInfo incoming = manager.getPackageArchiveInfo(target.getPath(), flags);
            if (incoming == null || incoming.applicationInfo == null) throw new IOException("Android could not parse this APK.");
            long code = versionCode(incoming);
            ApkUpdatePolicy.check(installed.packageName, versionCode(installed), signers(installed), incoming.packageName,
                    code, signers(incoming), incoming.applicationInfo.minSdkVersion, Build.VERSION.SDK_INT,
                    incoming.splitNames != null && incoming.splitNames.length != 0);
            Uri uri = new Uri.Builder().scheme("content").authority(context.getPackageName() + ".updates").appendPath(target.getName()).build();
            accepted = true;
            return new Candidate(target, incoming.versionName == null ? Long.toString(code) : incoming.versionName, code, uri);
        } finally { if (!accepted) { target.setWritable(true); target.delete(); } }
    }

    private static long versionCode(PackageInfo info) { return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode; }
    private static Set<String> signers(PackageInfo info) throws Exception {
        Signature[] signatures = Build.VERSION.SDK_INT >= 28 && info.signingInfo != null ? info.signingInfo.getApkContentsSigners() : info.signatures;
        Set<String> result = new HashSet<>();
        if (signatures != null) for (Signature signature : signatures) {
            byte[] hash = MessageDigest.getInstance("SHA-256").digest(signature.toByteArray());
            StringBuilder text = new StringBuilder();
            for (byte value : hash) text.append(String.format(Locale.ROOT, "%02x", value & 255));
            result.add(text.toString());
        }
        return result;
    }
}
