/**
 * Node 18 compatibility polyfill for Metro and React Native tools on Vega OS SDK
 */
const util = require('util');

if (!Array.prototype.toReversed) {
  Array.prototype.toReversed = function() {
    return [...this].reverse();
  };
}
if (!Array.prototype.toSorted) {
  Array.prototype.toSorted = function(compareFn) {
    return [...this].sort(compareFn);
  };
}
if (!Array.prototype.toSpliced) {
  Array.prototype.toSpliced = function(start, deleteCount, ...items) {
    const copy = [...this];
    copy.splice(start, deleteCount, ...items);
    return copy;
  };
}
if (!Array.prototype.with) {
  Array.prototype.with = function(index, value) {
    const copy = [...this];
    copy[index] = value;
    return copy;
  };
}
if (!util.styleText) {
  util.styleText = (format, text) => text;
}
