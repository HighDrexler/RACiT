# Sourced by mac/build_launcher.sh and mac/package_macos.sh (bash 3.2, as macOS has).
# bundle_dylibs BINARY LIBDIR RPATH [EXTRA...]
#   Copies every non-system library BINARY loads (and what those load in turn, and the EXTRA
#   ones it dlopen()s) into LIBDIR, names each @rpath/<file>, gives each library @loader_path
#   as its only rpath and BINARY only RPATH (e.g. @executable_path/lib): nothing points at
#   Homebrew afterwards. An EXTRA written SRC=NAME is copied as NAME (sdl2-compat dlopens
#   @loader_path/libSDL3.dylib).
# MACOS_LIB_PREFIX (optional): a prefix whose lib/ is searched first for every library, by file
#   name, even one BINARY names by an absolute Homebrew path -- e.g. libraries built for an older
#   macOS than Homebrew's bottles (mac/package_macos.sh, mac/build_launcher.sh).

_is_system() { case "$1" in /usr/lib/*|/System/*) return 0;; *) return 1;; esac; }
# Paths may hold spaces (a MACOS_LIB_PREFIX like "~/My Libs"): one per line, never split on spaces.
_rpaths() { otool -l "$1" | awk '/cmd LC_RPATH/{getline; getline; l=$0; sub(/^[ \t]*path /,"",l); sub(/ \(offset [0-9]+\)$/,"",l); print l}'; }
# The libraries FILE loads, one per line: otool -L without the "(compatibility version ...)" part.
_deps() { otool -L "$1" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \((compatibility|current) version [^)]*\)$//'; }

_ORIGINS=""   # "name<TAB>real path" lines: where each copied library came from
_origin() {
    local n; n="$(basename "$1")"
    local o; o="$(printf '%s' "$_ORIGINS" | awk -F'\t' -v n="$n" '$1==n{print $2; exit}')"
    if [ -n "$o" ]; then echo "$o"; else realpath "$1"; fi
}

# The file DEP names, as loaded from FROM (the real path of the library that loads it).
_resolve() {
    local dep="$1" from="$2" base dir r
    base="$(basename "$dep")"; dir="$(dirname "$from")"
    if [ -n "$MACOS_LIB_PREFIX" ] && [ -f "$MACOS_LIB_PREFIX/lib/$base" ]; then realpath "$MACOS_LIB_PREFIX/lib/$base"; return 0; fi
    case "$dep" in
        @rpath/*|@loader_path/*|@executable_path/*)
            while IFS= read -r r; do
                [ -n "$r" ] && [ -f "$r/$base" ] && { realpath "$r/$base"; return 0; }
            done < <(printf '%s\n' "$dir"; _rpaths "$from" | while IFS= read -r x; do printf '%s\n' "${x/@loader_path/$dir}"; done
                     printf '%s\n' /opt/homebrew/lib /usr/local/lib)
            echo "macos_dylibs: cannot find $dep (loaded by $from)" >&2; return 1;;
        *) [ -f "$dep" ] || { echo "macos_dylibs: missing $dep" >&2; return 1; }; realpath "$dep";;
    esac
}

_QUEUE=()
_take() {   # LIBDIR NAME REALPATH: copy once, queue it
    [ -f "$1/$2" ] && return 0
    cp -f "$3" "$1/$2" && chmod u+w "$1/$2" || return 1
    _ORIGINS="$_ORIGINS$2	$3
"
    _QUEUE+=("$1/$2")
}

# FILE's non-system load commands -> @rpath/<name>, each library copied into LIBDIR.
_relink() {
    local file="$1" libdir="$2" self dep name real
    self="$(otool -D "$file" | tail -n +2 | head -1)"
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        _is_system "$dep" && continue
        [ "$dep" = "$self" ] && continue
        name="$(basename "$dep")"
        if [ ! -f "$libdir/$name" ]; then
            real="$(_resolve "$dep" "$(_origin "$file")")" || return 1
            _take "$libdir" "$name" "$real" || return 1
        fi
        [ "$dep" != "@rpath/$name" ] && install_name_tool -change "$dep" "@rpath/$name" "$file" 2>/dev/null
    done < <(_deps "$file")
    return 0
}

bundle_dylibs() {
    local bin="$1" libdir="$2" rpath="$3" extra src as f r
    shift 3
    mkdir -p "$libdir"
    _QUEUE=(); _ORIGINS=""
    _ORIGINS="$(basename "$bin")	$(realpath "$bin")
"
    for extra in "$@"; do
        src="${extra%%=*}"; as="${extra#*=}"
        [ "$as" = "$extra" ] && as="$(basename "$src")"
        _take "$libdir" "$as" "$(realpath "$src")" || return 1
    done
    _relink "$bin" "$libdir" || return 1
    while [ ${#_QUEUE[@]} -gt 0 ]; do
        f="${_QUEUE[0]}"; _QUEUE=("${_QUEUE[@]:1}")
        install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null
        _relink "$f" "$libdir" || return 1
        while IFS= read -r r; do install_name_tool -delete_rpath "$r" "$f" 2>/dev/null; done < <(_rpaths "$f")
        install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null
    done
    while IFS= read -r r; do install_name_tool -delete_rpath "$r" "$bin" 2>/dev/null; done < <(_rpaths "$bin")
    install_name_tool -add_rpath "$rpath" "$bin"
}

# What in FILE still names a library outside the system and the bundle (should be nothing).
foreign_deps() {
    _deps "$1" | while IFS= read -r d; do
        _is_system "$d" && continue
        case "$d" in @rpath/*) ;; *) echo "$d";; esac
    done
    _rpaths "$1" | grep -v -e '^@loader_path' -e '^@executable_path' || true
}
