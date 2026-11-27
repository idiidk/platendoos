import DefaultTheme from 'vitepress/theme'
import type { Theme } from 'vitepress'
import { h } from 'vue'
import Trailer from './Trailer.vue'
import ModInfo from './ModInfo.vue'
import EditionBadge from './EditionBadge.vue'
import Coffee from './Coffee.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () => h(DefaultTheme.Layout, null, {
      'nav-bar-title-after': () => h(EditionBadge),
      'home-hero-actions-after': () => h(Coffee),
    }),
  enhanceApp({ app }) {
    app.component('Trailer', Trailer)
    app.component('ModInfo', ModInfo)
  },
} satisfies Theme
