<!-- The home page's coffee line, under the hero's buttons: a record that spins at 33 rpm with a coffee cup on its
     label, and at 45 when you point at it. -->
<template>
  <a class="coffee" href="https://buymeacoffee.com/idiidk" target="_blank" rel="noopener">
    <span class="record" aria-hidden="true">
      <span class="label">☕</span>
    </span>
    <span class="words">
      <span class="idle">Runs on coffee and loud music.</span>
      <b>Buy me a coffee</b>
    </span>
  </a>
</template>

<style scoped>
.coffee {
  display: inline-flex;
  align-items: center;
  gap: 12px;
  margin-top: 22px;
  padding: 6px 18px 6px 6px;
  border: 1px dashed var(--vp-c-divider);
  border-radius: 999px;
  font-size: 14px;
  line-height: 20px;
  color: var(--vp-c-text-2);
  text-decoration: none;
  transition: border-color 0.25s, background-color 0.25s;
}
.coffee:hover {
  border-color: #c98a3c;
  background: rgba(201, 138, 60, 0.1);
}
.words {
  display: flex;
  flex-direction: column;
  text-align: left;
}
.coffee b {
  color: var(--vp-c-text-1);
  font-weight: 600;
}
.coffee:hover b {
  color: #c98a3c;
}
/* the record: grooves, a sheen that turns with it, and the label */
.record {
  position: relative;
  flex: none;
  width: 52px;
  height: 52px;
  border-radius: 50%;
  background:
    conic-gradient(from 20deg, rgba(255, 255, 255, 0) 0 12%, rgba(255, 255, 255, 0.22) 18%, rgba(255, 255, 255, 0) 24% 62%, rgba(255, 255, 255, 0.22) 68%, rgba(255, 255, 255, 0) 74%),
    repeating-radial-gradient(circle, #151515 0 1.5px, #262626 1.5px 2.5px);
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.35);
  animation: spin 1.82s linear infinite; /* 33 rpm */
}
.coffee:hover .record {
  animation-duration: 1.33s; /* 45 rpm */
}
.label {
  position: absolute;
  inset: 14px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  background: #c98a3c;
  font-size: 14px;
  line-height: 1;
}
@keyframes spin {
  to { transform: rotate(360deg); }
}
@media (prefers-reduced-motion: reduce) {
  .record { animation: none; }
}
@media (max-width: 480px) {
  .idle { display: none; }
}
</style>
