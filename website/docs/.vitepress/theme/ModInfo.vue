<script setup lang="ts">
// The facts box at the top of a mod page: platform, group, switch and what it works with.
// In `needs`, [name] links to that mod's page.
import { computed } from 'vue'
import { withBase } from 'vitepress'
const props = defineProps<{
  platform?: 'both' | 'mac' | 'win'
  group: string
  name: string
  needs?: string
  off?: string
}>()
const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
const needsHtml = computed(() =>
  props.needs ? esc(props.needs).replace(/\[(\w+)\]/g, (_, m) => `<a href="${withBase(`/mods/${m}.html`)}">${m}</a>`) : '',
)
</script>

<template>
  <div class="pd-modinfo">
    <div><span class="k">Platform</span>
      <span v-if="!platform || platform === 'both'" class="v">macOS and Windows</span>
      <span v-else-if="platform === 'mac'" class="v"><span class="pd-tag">mac</span> macOS only</span>
      <span v-else class="v"><span class="pd-tag">win</span> Windows only</span>
    </div>
    <div><span class="k">Group</span><span class="v">{{ group }}</span></div>
    <div><span class="k">Switch</span><span class="v"><code>{{ off || name + '=0' }}</code> turns it off (restart rekordbox)</span></div>
    <div v-if="needs"><span class="k">Works with</span><span class="v" v-html="needsHtml"></span></div>
  </div>
</template>
