import next from "eslint-config-next";

const config = [
  ...next,
  {
    rules: {
      // Effects here reset view state when the selected dataset/run changes;
      // that is the intended behaviour, so this stays a warning.
      "react-hooks/set-state-in-effect": "warn",
    },
  },
  { ignores: [".next/**", "node_modules/**", "playwright-report/**", "test-results/**"] },
];
export default config;
