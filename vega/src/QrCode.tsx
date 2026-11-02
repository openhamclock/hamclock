/**
 * Draws a QR code from the engine's module grid (HcEngine.qrMatrix) with plain Views, so the app
 * needs no QR or SVG dependency.
 *
 * Each row is one View; runs of same-colored modules are merged into one child, which keeps the
 * view count low (a few hundred for a version-3 code). The module size is snapped to whole device
 * pixels so no seams appear between modules.
 */
import React, {useMemo} from 'react';
import {PixelRatio, StyleSheet, View} from 'react-native';

/** Quiet zone width in modules (the QR spec asks for 4). */
const QUIET = 4;

interface Run {
  dark: boolean;
  n: number;
}

function rowRuns(row: boolean[]): Run[] {
  const runs: Run[] = [];
  for (const dark of row) {
    const last = runs[runs.length - 1];
    if (last && last.dark === dark) {
      last.n += 1;
    } else {
      runs.push({dark, n: 1});
    }
  }
  return runs;
}

/**
 * @param matrix rows of modules, true = dark (no quiet zone)
 * @param size   target outer size in layout units, quiet zone included; the drawn size is
 *               rounded down to a whole number of device pixels per module
 */
export function QrCode({matrix, size}: {matrix: boolean[][]; size: number}) {
  const runs = useMemo(() => matrix.map(rowRuns), [matrix]);
  if (matrix.length === 0) {
    return null;
  }
  const modules = matrix.length + 2 * QUIET;
  const scale = PixelRatio.get();
  const modPx = Math.max(1, Math.floor((size * scale) / modules));
  const mod = modPx / scale;
  return (
    <View
      accessibilityLabel="QR code"
      style={[styles.light, {padding: QUIET * mod, width: modules * mod, height: modules * mod}]}>
      {runs.map((row, y) => (
        <View key={y} style={[styles.row, {height: mod}]}>
          {row.map((r, i) => (
            <View key={i} style={[r.dark ? styles.dark : styles.light, {width: r.n * mod, height: mod}]} />
          ))}
        </View>
      ))}
    </View>
  );
}

const styles = StyleSheet.create({
  row: {flexDirection: 'row'},
  dark: {backgroundColor: '#000000'},
  light: {backgroundColor: '#ffffff'},
});

export default QrCode;
