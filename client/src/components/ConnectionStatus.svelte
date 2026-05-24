<script>
  import { connectionStatus } from '../store'
  import { recheckConnection } from '../helper/connectionStatus'
  import ThemeToggle from './ThemeToggle.svelte'

  $: cls = 'status-dot ' + $connectionStatus
  $: label =
    $connectionStatus === 'online' ? 'Connected' :
    $connectionStatus === 'offline' ? 'Blocked — click to retry' :
    $connectionStatus === 'checking' ? 'Checking…' : 'Unknown'
</script>

<div class="flex items-center gap-2">
  <button type="button" class="btn btn-ghost flex items-center gap-2" title={label} on:click={recheckConnection}>
    <span class={cls}></span>
    <span class="text-xs">{label}</span>
  </button>
  <ThemeToggle />
</div>
