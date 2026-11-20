<script setup lang="ts">
import { computed } from 'vue'
import { useData, withBase } from 'vitepress'
const { theme } = useData()
const src = computed(() => {
  const t = (theme.value as { trailer?: string }).trailer || ''
  return t.startsWith('/') ? withBase(t) : t
})
</script>

<template>
  <figure class="pd-trailer">
    <video :src="src" controls preload="none" playsinline :poster="withBase('/img/quickdeck.webp')"></video>
    <figcaption>
      The trailer (with sound), recorded in rekordbox. If it doesn't play here,
      <a :href="src">open the video</a>.
    </figcaption>
  </figure>
</template>
