<script>
  import { useDispatch } from '../use/dispatch'

  const { dispatch } = useDispatch()
  export let title = 'Drawer'
  export let fullscreen = false  // kept for API compat; native modal is always full

  let isOpen = false

  export function show() {
    isOpen = true
    dispatch('show')
  }

  export function hide() {
    isOpen = false
    dispatch('after-hide')
  }

  function onBackdropClick(e) {
    if (e.target === e.currentTarget) hide()
  }

  function onKey(e) {
    if (e.key === 'Escape' && isOpen) hide()
  }
</script>

<svelte:window on:keydown={onKey} />

{#if isOpen}
  <div class="drawer-backdrop" on:click={onBackdropClick}>
    <div class="drawer-panel">
      <header class="drawer-header">
        <h2 style="font-size:18px;font-weight:600;color:var(--ink);margin:0">{title}</h2>
        <button type="button" class="btn btn-ghost" on:click={hide} aria-label="Close">✕</button>
      </header>
      <div class="drawer-body">
        <slot />
      </div>
      <div class="drawer-footer">
        <button type="button" class="btn btn-secondary" on:click={hide}>Close</button>
        <slot name="footer" />
      </div>
    </div>
  </div>
{/if}

<style>
  .drawer-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    backdrop-filter: blur(4px);
    -webkit-backdrop-filter: blur(4px);
    z-index: 1000;
    display: flex;
    align-items: stretch;
    justify-content: stretch;
  }
  .drawer-panel {
    width: 100%;
    height: 100%;
    background: var(--bg-surface);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }
  .drawer-header {
    padding: 18px 22px 14px;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .drawer-body {
    flex: 1;
    overflow: auto;
    padding: 18px 22px;
  }
  .drawer-footer {
    padding: 14px 22px;
    border-top: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 12px;
  }
</style>
