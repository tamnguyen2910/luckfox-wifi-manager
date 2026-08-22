import QtQuick 2.15

QtObject {
    // ── Mode ────────────────────────────────────────────────────────
    // Toggled from the header: theme.dark = !theme.dark
    property bool dark: true

    // ── Palette ── "Graphite" (dark) / "Porcelain" (light) ──────────
    // Deep neutral surfaces — no navy/purple cast. One confident cyan
    // accent, used sparingly. Disciplined semantic status colors.
    readonly property color bg:         dark ? "#0C0F13" : "#F3F4F6"
    readonly property color bgSubtle:   dark ? "#11151B" : "#E9EBEE"
    readonly property color surface:    dark ? "#161B23" : "#FFFFFF"
    readonly property color surface2:   dark ? "#1C222C" : "#F5F6F8"
    readonly property color surface3:   dark ? "#242C38" : "#E8EAEE"
    readonly property color overlay:    dark ? "#131820" : "#FFFFFF"

    // Hairline borders
    readonly property color border:       dark ? "#242C37" : "#DEE1E6"
    readonly property color borderHi:     dark ? "#333D4B" : "#C8CDD4"
    readonly property color borderAccent: dark ? "#0E7490" : "#0E7490"

    // Text — 4-step hierarchy
    readonly property color text1:   dark ? "#E9EBEE" : "#191C20"
    readonly property color text2:   dark ? "#98A1AC" : "#59616B"
    readonly property color text3:   dark ? "#6E7883" : "#6B7280"
    readonly property color textDim: dark ? "#49525D" : "#9AA1A9"

    // Accent — cyan
    readonly property color accent:     dark ? "#06B6D4" : "#0891B2"
    readonly property color accentHi:   dark ? "#67E8F9" : "#0E7490"
    readonly property color accentDk:   dark ? "#0891B2" : "#155E75"
    readonly property color accentSoft: dark ? "#0D2730" : "#D9F1F6"

    // Ink — pure white marks that stay white in both modes
    readonly property color ink: "#FFFFFF"

    // Semantic — success / warning / danger
    readonly property color green:       dark ? "#34D399" : "#059669"
    readonly property color greenSoft:   dark ? "#11251D" : "#DCF5EA"
    readonly property color greenBorder: dark ? "#1F4A38" : "#9BDCC2"

    readonly property color amber:       dark ? "#FBBF24" : "#B45309"
    readonly property color amberSoft:   dark ? "#2A2110" : "#FCF0D4"
    readonly property color amberBorder: dark ? "#4D3B14" : "#EBCF8E"

    readonly property color red:       dark ? "#F87171" : "#DC2626"
    readonly property color redSoft:   dark ? "#2A1417" : "#FDECEC"
    readonly property color redBorder: dark ? "#55262B" : "#F3B9B9"
    readonly property color redDk:     dark ? "#7F1D1D" : "#B91C1C"
    readonly property color redText:   dark ? "#FFD9D6" : "#9F1D1D"
    readonly property color redTextHi: dark ? "#FFB3AE" : "#7F1D1D"
    readonly property color weakText:  dark ? "#FFB3AD" : "#B91C1C"

    // Scrim behind modal sheets
    readonly property color shadow: "#000000"

    // ── Radius scale ────────────────────────────────────────────────
    readonly property int rXs: 4
    readonly property int rSm: 8
    readonly property int rMd: 12
    readonly property int rLg: 16
    readonly property int rXl: 20
    readonly property int rPill: 999

    // ── Spacing scale ───────────────────────────────────────────────
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 20
    readonly property int s6: 24
    readonly property int s7: 32

    // ── Typography (DejaVu only on Luckfox) ─────────────────────────
    readonly property string fontFamily: "DejaVu Sans"
    readonly property string fontMono:   "DejaVu Sans Mono"
}
