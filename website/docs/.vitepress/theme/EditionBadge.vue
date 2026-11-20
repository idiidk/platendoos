<script setup lang="ts">
// A small label next to the site title, set by the full edition's private/website/site.mjs (badge).
import { useData } from 'vitepress'
const { theme } = useData()
</script>

<template>
  <span v-if="theme.editionBadge" class="pd-edition">{{ theme.editionBadge }}</span>
</template>
