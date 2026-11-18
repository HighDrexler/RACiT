package com.racacit.recomp;

/** Geometry only: directions are controller input, never inventory positions. */
final class CompanionRadial {
    private CompanionRadial() { }
    static int direction(float x, float y, float innerRadius, float outerRadius) {
        if (!Float.isFinite(x) || !Float.isFinite(y) || innerRadius < 0 || outerRadius <= innerRadius) return -1;
        double distance = Math.hypot(x, y);
        if (distance < innerRadius || distance > outerRadius) return -1;
        double angle = Math.atan2(y, x) + Math.PI / 2 + Math.PI / 8;
        return ((int)Math.floor(angle / (Math.PI / 4)) + 8) & 7;
    }
    static float x(int direction) { return (float)Math.sin(direction * Math.PI / 4); }
    static float y(int direction) { return -(float)Math.cos(direction * Math.PI / 4); }
}
