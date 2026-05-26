<template>
  <span class="status-pill" :class="toneClass">
    <span class="dot" />
    {{ label }}
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  value?: string | boolean | null
}>()

const normalized = computed(() => {
  if (typeof props.value === 'boolean') return props.value ? 'online' : 'offline'
  return String(props.value || 'unknown').toLowerCase()
})

const label = computed(() => normalized.value)

const toneClass = computed(() => {
  if (['ok', 'online', 'normal', 'up', 'true', 'present'].includes(normalized.value)) return 'good'
  if (['warn', 'warning', 'stale'].includes(normalized.value)) return 'warn'
  if (['error', 'offline', 'down', 'bus-off', 'false', 'missing', 'absent'].includes(normalized.value)) return 'bad'
  return 'muted'
})
</script>
