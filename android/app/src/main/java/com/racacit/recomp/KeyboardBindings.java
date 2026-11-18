package com.racacit.recomp;

import java.util.LinkedHashMap;
import java.util.Map;

/** Windows/v97 native button names and SDL key-name format; unknown user keys are retained. */
final class KeyboardBindings {
    static final String[][] ALL = {
        { "CROSS", "Cross: jump", "Space,Return" }, { "CIRCLE", "Circle: back / action", "E,Backspace" },
        { "SQUARE", "Square: melee", "F" }, { "TRIANGLE", "Triangle: quick select", "Q" },
        { "L1", "L1: strafe", "Mouse3" }, { "R1", "R1: fire", "Mouse1" },
        { "L2", "L2", "Left Ctrl" }, { "R2", "R2", "Left Shift" }, { "L3", "L3", "V" },
        { "R3", "R3", "Mouse2,B" }, { "START", "Start: pause", "Escape,P" }, { "SELECT", "Select: map", "Tab,M" },
        { "UP", "D-pad up", "Up,1" }, { "DOWN", "D-pad down", "Down,3" },
        { "LEFT", "D-pad left", "Left,4" }, { "RIGHT", "D-pad right", "Right,2" },
        { "LS_UP", "Move forward", "W" }, { "LS_DOWN", "Move backward", "S" },
        { "LS_LEFT", "Move left", "A" }, { "LS_RIGHT", "Move right", "D" }, { "WALK", "Walk (hold)", "Left Alt" },
        { "RS_UP", "Camera up", "I" }, { "RS_DOWN", "Camera down", "K" },
        { "RS_LEFT", "Camera left", "J" }, { "RS_RIGHT", "Camera right", "L" }
    };

    /** Null means restore the native default. The runtime remains the authority for supported SDL names. */
    static String normalize(String input) {
        if (input == null) return null;
        if (input.indexOf('\n') >= 0 || input.indexOf('\r') >= 0 || input.indexOf(0) >= 0)
            throw new IllegalArgumentException("Use one line of key names.");
        if (input.trim().isEmpty()) return null;
        StringBuilder out = new StringBuilder(); int count = 0;
        for (String part : input.split(",")) {
            String key = part.trim();
            if (key.isEmpty()) continue;
            if (++count > 4) throw new IllegalArgumentException("The game accepts at most four keys per button.");
            if (out.length() > 0) out.append(',');
            out.append(key);
        }
        if (out.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 240)
            throw new IllegalArgumentException("This binding is too long for the native key parser.");
        return out.length() == 0 ? null : out.toString();
    }

    static Map<String, String> defaults() {
        Map<String, String> changes = new LinkedHashMap<>();
        for (String[] row : ALL) changes.put("RACACIT_BIND_" + row[0], null);
        return changes;
    }
}
