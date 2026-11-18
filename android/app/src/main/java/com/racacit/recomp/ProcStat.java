package com.racacit.recomp;

/** Small bounded parser for proc stat fields14,15,22; handles spaces/parentheses in comm. */
final class ProcStat {
    final String name;
    final long ticks, started;
    ProcStat(String name, long ticks, long started) { this.name = name; this.ticks = ticks; this.started = started; }
    static ProcStat parse(String text) {
        if (text == null) return null;
        int open = text.indexOf('('), close = text.lastIndexOf(')');
        if (open < 0 || close <= open) return null;
        long user = -1, kernel = -1, start = -1; int position = close + 1;
        try {
            for (int field = 3; field <= 22; field++) {
                while (position < text.length() && Character.isWhitespace(text.charAt(position))) position++;
                int begin = position;
                while (position < text.length() && !Character.isWhitespace(text.charAt(position))) position++;
                if (begin == position) return null;
                if (field == 14) user = Long.parseLong(text.substring(begin, position));
                if (field == 15) kernel = Long.parseLong(text.substring(begin, position));
                if (field == 22) start = Long.parseLong(text.substring(begin, position));
            }
            if (user < 0 || kernel < 0 || start < 0 || user > Long.MAX_VALUE - kernel) return null;
            return new ProcStat(text.substring(open + 1, close), user + kernel, start);
        } catch (NumberFormatException invalid) { return null; }
    }
    static float percent(ProcStat now, ProcStat previous, long elapsedMs, long ticksPerSecond) {
        if (now == null || previous == null || now.started != previous.started || now.ticks < previous.ticks || elapsedMs <= 0 || ticksPerSecond <= 0)
            return Float.NaN;
        double percent = (now.ticks - previous.ticks) * 100000d / (elapsedMs * (double) ticksPerSecond);
        return Double.isFinite(percent) && percent <= Float.MAX_VALUE ? (float) percent : Float.NaN;
    }
}
