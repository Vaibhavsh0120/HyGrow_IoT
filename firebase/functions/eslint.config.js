// Minimal flat config (ESLint 9+) — just enough to catch obvious mistakes
// (undefined vars, unused vars) in index.js without imposing a full style
// guide. Run with `npm run lint` inside functions/.
"use strict";

module.exports = [
  {
    files: ["**/*.js"],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: "commonjs",
      globals: {
        require: "readonly",
        module: "readonly",
        exports: "writable",
        process: "readonly",
        console: "readonly",
      },
    },
    rules: {
      "no-unused-vars": "warn",
      "no-undef": "error",
    },
  },
];
