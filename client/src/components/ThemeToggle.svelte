<script>
  import { onMount } from 'svelte'

  // theme: 'system' | 'dark' | 'light'
  let theme = 'system'

  function applyTheme(t) {
    const html = document.documentElement
    html.classList.remove('light', 'dark')
    if (t === 'light') html.classList.add('light')
    else if (t === 'dark') html.classList.add('dark')
    // 'system' = no class, falls through to prefers-color-scheme media query
  }

  function cycle() {
    theme = theme === 'system' ? 'dark' : theme === 'dark' ? 'light' : 'system'
    try {
      localStorage.setItem('theme', theme)
    } catch (e) {}
    applyTheme(theme)
  }

  onMount(() => {
    try {
      const saved = localStorage.getItem('theme')
      if (saved === 'dark' || saved === 'light' || saved === 'system') {
        theme = saved
      }
    } catch (e) {}
    applyTheme(theme)
  })

  // tooltip label
  $: label =
    theme === 'system'
      ? 'System theme (click to switch to dark)'
      : theme === 'dark'
      ? 'Dark mode (click to switch to light)'
      : 'Light mode (click to switch to system)'
</script>

<button
  type="button"
  class="theme-toggle"
  title={label}
  on:click={cycle}
  aria-label={label}
>
  {#if theme === 'light'}
    <!-- sun icon -->
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <circle cx="12" cy="12" r="4" />
      <path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41" />
    </svg>
  {:else if theme === 'dark'}
    <!-- moon icon -->
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z" />
    </svg>
  {:else}
    <!-- system icon (small grid) -->
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
      <rect x="3" y="4" width="18" height="14" rx="2" />
      <path d="M8 20h8M12 18v2" />
    </svg>
  {/if}
</button>
