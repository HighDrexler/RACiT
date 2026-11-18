package com.racacit.recomp;

import java.io.IOException;
import java.util.Set;

/** Identity policy before handing a local APK to Android's signature-verifying system installer. */
final class ApkUpdatePolicy {
    static void check(String installedPackage, long installedVersion, Set<String> installedSigners,
                      String incomingPackage, long incomingVersion, Set<String> incomingSigners,
                      int minimumSdk, int deviceSdk, boolean split) throws IOException {
        if (installedPackage == null || !installedPackage.equals(incomingPackage))
            throw new IOException("This APK is not an update for this independent Android app.");
        if (incomingVersion <= installedVersion) throw new IOException("Select a newer APK; this version is already installed or older.");
        if (installedSigners == null || installedSigners.isEmpty() || !installedSigners.equals(incomingSigners))
            throw new IOException("The APK signing identity does not match this installation.");
        if (minimumSdk > deviceSdk) throw new IOException("This APK needs a newer Android version.");
        if (split) throw new IOException("Select a complete APK, not a split-package component.");
    }
}
