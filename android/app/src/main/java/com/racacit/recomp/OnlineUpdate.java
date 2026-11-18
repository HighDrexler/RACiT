package com.racacit.recomp;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * 0.1.0: new Android releases from the project's GitHub page. The release list
 * (api.github.com, no sign-in) is read newest first; the first published release whose tag starts with
 * "android" and that carries an .apk is the candidate. Windows releases share the repository and are
 * skipped. Only a download address on this repository's own releases is accepted. The APK then goes
 * through OfflineUpdate.stage (package, version and signing identity checked) and Android's installer,
 * which asks the player again.
 */
final class OnlineUpdate {
    static final String API = "https://api.github.com/repos/HighDrexler/RACiT/releases?per_page=30";
    static final String DOWNLOADS = "https://github.com/HighDrexler/RACiT/releases/download/";

    static final class Release {
        String tag, name, notes, url;
        long size;
    }

    /** The newest Android release, or null if there is none. Throws on network trouble. */
    static Release latest() throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(API).openConnection();
        c.setConnectTimeout(8000); c.setReadTimeout(10000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
        c.setRequestProperty("User-Agent", "RACiT-Android-updater");
        try {
            if (c.getResponseCode() != 200) throw new IOException("GitHub answered " + c.getResponseCode());
            String body;
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] b = new byte[16384]; int n; long total = 0;
                while ((n = in.read(b)) != -1) { total += n; if (total > (8L << 20)) throw new IOException("GitHub's answer is too large"); out.write(b, 0, n); }
                body = out.toString("UTF-8");
            }
            JSONArray all = new JSONArray(body);
            for (int i = 0; i < all.length(); i++) {
                JSONObject r = all.getJSONObject(i);
                if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue;
                String tag = r.optString("tag_name", "");
                if (!tag.toLowerCase().startsWith("android")) continue;
                JSONArray assets = r.optJSONArray("assets");
                if (assets == null) continue;
                for (int k = 0; k < assets.length(); k++) {
                    JSONObject a = assets.getJSONObject(k);
                    String name = a.optString("name", ""), url = a.optString("browser_download_url", "");
                    if (!name.toLowerCase().endsWith(".apk") || !url.startsWith(DOWNLOADS) || url.contains("..") || url.contains("@")) continue;
                    Release rel = new Release();
                    rel.tag = tag; rel.name = r.optString("name", tag); if (rel.name.isEmpty()) rel.name = tag;
                    String notes = r.optString("body", "").replace("\r", "").replaceAll("[#*`_]", "").trim();
                    rel.notes = notes.length() > 600 ? notes.substring(0, 600) + " ..." : notes;
                    rel.url = url; rel.size = a.optLong("size", 0);
                    return rel;
                }
            }
            return null;
        } finally { c.disconnect(); }
    }

    /** "android-v0.1.2" -> its numbers; compared with this app's versionName ("0.1.0"). >0 when a is newer. */
    static int compare(String a, String b) {
        long[] x = parts(a), y = parts(b);
        if (x.length == 0 || y.length == 0) return x.length == 0 ? (y.length == 0 ? 0 : -1) : 1;
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            long p = i < x.length ? x[i] : 0, q = i < y.length ? y[i] : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }
    private static long[] parts(String s) {
        int i = 0;
        while (i < s.length() && !Character.isDigit(s.charAt(i))) i++;
        java.util.List<Long> v = new java.util.ArrayList<>();
        while (i < s.length() && Character.isDigit(s.charAt(i))) {
            long n = 0;
            while (i < s.length() && Character.isDigit(s.charAt(i)) && n < 100000000L) n = n * 10 + (s.charAt(i++) - '0');
            v.add(n);
            if (i + 1 < s.length() && s.charAt(i) == '.' && Character.isDigit(s.charAt(i + 1))) i++; else break;
        }
        long[] r = new long[v.size()];
        for (int k = 0; k < r.length; k++) r[k] = v.get(k);
        return r;
    }

    /** The APK's bytes (GitHub redirects to its file host; HttpURLConnection follows https->https). */
    static InputStream open(Release rel) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(rel.url).openConnection();
        c.setConnectTimeout(10000); c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent", "RACiT-Android-updater");
        if (c.getResponseCode() != 200) { c.disconnect(); throw new IOException("The download answered " + c.getResponseCode()); }
        if (!"https".equals(c.getURL().getProtocol())) { c.disconnect(); throw new IOException("The download left HTTPS"); }
        return c.getInputStream();
    }
}
