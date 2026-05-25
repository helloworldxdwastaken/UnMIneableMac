<script>
  import { tryOnMount } from '@svelte-use/core'
  import { form, cpuCores } from '../store'
  import { useDispatch } from '../use/dispatch'

  const { dispatch } = useDispatch()
  $: step = Math.max(1, Math.round(100 / $cpuCores))
  $: threadsAtCurrent = Math.max(1, Math.round((tweakForm.cpuUsage / 100) * $cpuCores))
  $: pCoreHint = $cpuCores >= 8 && tweakForm.cpuUsage <= 50
    ? 'P-cores only ✓ (best for RandomX)'
    : tweakForm.cpuUsage > 50 ? 'E-cores included — may reduce hashrate' : ''

  let tweakForm = { cpuUsage: $form.cpuUsage, useGPU: !!$form.useGPU }
  let formEl

  $: isVerus = $form.algorithm === 'verushash'

  export function getFormData() { return new FormData(formEl) }
  export function setFormData(data) {
    if (!data) return
    if (typeof data.cpuUsage === 'number' || typeof data.cpuUsage === 'string')
      tweakForm.cpuUsage = Number(data.cpuUsage)
    if (typeof data.useGPU === 'boolean') tweakForm.useGPU = data.useGPU
  }

  function onSlide(e) {
    tweakForm.cpuUsage = Number(e.target.value)
    dispatch('change', { ...$form, cpuUsage: tweakForm.cpuUsage, useGPU: tweakForm.useGPU })
  }

  function onGPUToggle(e) {
    tweakForm.useGPU = e.target.checked
    dispatch('change', { ...$form, cpuUsage: tweakForm.cpuUsage, useGPU: tweakForm.useGPU })
  }
</script>

<form bind:this={formEl}>
  <div class="form-group">
    <div class="flex items-center justify-between mb-2">
      <span class="label" style="margin-bottom:0">CPU Usage</span>
      <span class="mono text-accent" style="font-size:20px;font-weight:600">{tweakForm.cpuUsage}%</span>
    </div>
    <input type="range" name="cpuUsage" min={step} max="100" {step} value={tweakForm.cpuUsage}
      on:input={onSlide}
      style="width:100%;accent-color:var(--accent)"/>
    <div class="flex justify-between text-xs text-dim mt-1">
      <span>{step}%</span>
      <span>~{threadsAtCurrent} / {$cpuCores} threads</span>
      <span>100%</span>
    </div>
    {#if pCoreHint}
      <p class="text-xs mt-2 text-dim">{pCoreHint}</p>
    {/if}
  </div>

  {#if isVerus}
    <div class="form-group" style="margin-top:16px">
      <label style="display:flex;align-items:flex-start;gap:10px;cursor:pointer">
        <input type="checkbox" name="useGPU" checked={tweakForm.useGPU}
               on:change={onGPUToggle}
               style="margin-top:3px;accent-color:var(--accent)" />
        <div>
          <div class="label" style="margin-bottom:2px">Use Metal GPU (experimental)</div>
          <div class="text-xs text-dim" style="line-height:1.4">
            VerusHash 2.2 on the M-series GPU. Target 12–20 MH/s vs ~4 MH/s on CPU.
            Trust-but-verify enabled: first 16 shares re-checked on CPU before submission.
            Falls back to CPU if Metal isn't available.
          </div>
        </div>
      </label>
    </div>
  {/if}
</form>
