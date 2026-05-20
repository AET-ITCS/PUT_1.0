<template>
  <main class="app-shell">
    <aside class="sidebar">
      <div class="brand">
        <Activity class="brand-icon" :size="26" />
        <div>
          <strong>PUT Monitor</strong>
          <span>{{ health?.version || '0.1.0' }}</span>
        </div>
      </div>

      <nav class="nav-list" aria-label="监控视图">
        <button
          v-for="item in navItems"
          :key="item.key"
          class="nav-button"
          :class="{ active: activeView === item.key }"
          type="button"
          :title="item.label"
          @click="activeView = item.key"
        >
          <component :is="item.icon" :size="18" />
          <span>{{ item.label }}</span>
        </button>
      </nav>
    </aside>

    <section class="workspace">
      <header class="topbar">
        <div>
          <h1>{{ currentTitle }}</h1>
          <p>{{ statusLine }}</p>
        </div>
        <div class="top-actions">
          <StatusPill :value="health?.status || 'unknown'" />
          <button class="icon-button" type="button" title="刷新" @click="refreshAll">
            <RefreshCw :size="18" />
          </button>
        </div>
      </header>

      <section v-if="activeView === 'dashboard'" class="view-grid dashboard-grid">
        <article class="metric-panel">
          <div class="panel-title">
            <Cpu :size="18" />
            <span>系统负载</span>
          </div>
          <strong>{{ percent(resources?.cpu.usage_percent) }}</strong>
          <small>CPU</small>
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <MemoryStick :size="18" />
            <span>内存</span>
          </div>
          <strong>{{ percent(resources?.memory.usage_percent) }}</strong>
          <small>{{ bytesFromKb(resources?.memory.used_kb) }} / {{ bytesFromKb(resources?.memory.total_kb) }}</small>
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <RadioTower :size="18" />
            <span>CAN</span>
          </div>
          <strong>{{ canStatus?.bus_state || 'unknown' }}</strong>
          <StatusPill :value="canStatus?.state" />
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <Waypoints :size="18" />
            <span>IPC</span>
          </div>
          <strong>{{ ipcStatus?.online ? 'online' : 'offline' }}</strong>
          <StatusPill :value="ipcStatus?.state" />
        </article>

        <article class="wide-panel">
          <div class="section-heading">
            <h2>模块状态</h2>
            <StatusPill :value="modules?.state" />
          </div>
          <div v-if="modules?.modules.length" class="module-strip">
            <div v-for="module in modules.modules" :key="module.name" class="module-tile">
              <div>
                <strong>{{ moduleLabel(module.name) }}</strong>
                <span>{{ module.message || 'no message' }}</span>
              </div>
              <StatusPill :value="module.status" />
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>

        <article class="wide-panel">
          <div class="section-heading">
            <h2>最近事件</h2>
            <StatusPill :value="events?.parse_error_count ? 'warn' : 'ok'" />
          </div>
          <div v-if="events?.events.length" class="event-list compact">
            <div v-for="event in events.events.slice(-5)" :key="`${event.timestamp_ms}-${event.message}`" class="event-row">
              <StatusPill :value="event.level" />
              <span>{{ event.source }}</span>
              <strong>{{ event.message }}</strong>
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>
      </section>

      <section v-else-if="activeView === 'modules'" class="view-stack">
        <div class="section-heading">
          <h2>协议模块</h2>
          <StatusPill :value="modules?.state" />
        </div>
        <div v-if="modules?.modules.length" class="data-table">
          <div class="table-row table-head">
            <span>模块</span><span>状态</span><span>RX</span><span>TX</span><span>错误</span><span>最近通信</span><span>消息</span>
          </div>
          <div v-for="module in modules.modules" :key="module.name" class="table-row">
            <strong>{{ moduleLabel(module.name) }}</strong>
            <StatusPill :value="module.status" />
            <span>{{ number(module.rx_count) }}</span>
            <span>{{ number(module.tx_count) }}</span>
            <span>{{ number(module.error_count) }}</span>
            <span>{{ ms(module.last_seen_ms) }}</span>
            <span>{{ module.message || 'no data' }}</span>
          </div>
        </div>
        <p v-else class="empty-text">no data</p>
      </section>

      <section v-else-if="activeView === 'resources'" class="view-stack">
        <div class="resource-grid">
          <article class="metric-panel">
            <div class="panel-title"><Cpu :size="18" /><span>CPU</span></div>
            <strong>{{ percent(resources?.cpu.usage_percent) }}</strong>
            <StatusPill :value="resources?.cpu.state" />
          </article>
          <article class="metric-panel">
            <div class="panel-title"><MemoryStick :size="18" /><span>内存</span></div>
            <strong>{{ percent(resources?.memory.usage_percent) }}</strong>
            <small>{{ bytesFromKb(resources?.memory.available_kb) }} available</small>
          </article>
          <article class="metric-panel">
            <div class="panel-title"><Clock3 :size="18" /><span>运行时间</span></div>
            <strong>{{ duration(resources?.uptime.uptime_seconds) }}</strong>
            <StatusPill :value="resources?.uptime.state" />
          </article>
        </div>

        <div class="section-heading"><h2>磁盘</h2></div>
        <div v-if="resources?.disks.length" class="data-table disk-table">
          <div class="table-row table-head"><span>挂载点</span><span>已用</span><span>容量</span><span>使用率</span></div>
          <div v-for="disk in resources.disks" :key="disk.mount_point" class="table-row">
            <strong>{{ disk.mount_point }}</strong>
            <span>{{ bytes(disk.used_bytes) }}</span>
            <span>{{ bytes(disk.total_bytes) }}</span>
            <span>{{ percent(disk.usage_percent) }}</span>
          </div>
        </div>
        <p v-else class="empty-text">unknown</p>

        <div class="section-heading"><h2>网络</h2></div>
        <div v-if="resources?.networks.length" class="data-table network-table">
          <div class="table-row table-head"><span>接口</span><span>状态</span><span>RX</span><span>TX</span></div>
          <div v-for="net in resources.networks" :key="net.name" class="table-row">
            <strong>{{ net.name }}</strong>
            <StatusPill :value="net.state" />
            <span>{{ bytes(net.rx_bytes) }}</span>
            <span>{{ bytes(net.tx_bytes) }}</span>
          </div>
        </div>
        <p v-else class="empty-text">unknown</p>
      </section>

      <section v-else-if="activeView === 'bus'" class="view-grid bus-grid">
        <article class="wide-panel">
          <div class="section-heading">
            <h2>CAN</h2>
            <StatusPill :value="canStatus?.state" />
          </div>
          <div class="stat-list">
            <span>bus_state <strong>{{ canStatus?.bus_state || 'unknown' }}</strong></span>
            <span>tx_count <strong>{{ number(canStatus?.tx_count) }}</strong></span>
            <span>rx_count <strong>{{ number(canStatus?.rx_count) }}</strong></span>
            <span>error_count <strong>{{ number(canStatus?.error_count) }}</strong></span>
            <span>drop_count <strong>{{ number(canStatus?.drop_count) }}</strong></span>
            <span>last_error <strong>{{ canStatus?.last_error || 'unknown' }}</strong></span>
          </div>
        </article>
        <article class="wide-panel">
          <div class="section-heading">
            <h2>IPC</h2>
            <StatusPill :value="ipcStatus?.state" />
          </div>
          <div class="stat-list">
            <span>online <strong>{{ ipcStatus?.online ? 'true' : 'false' }}</strong></span>
            <span>heartbeat_ms <strong>{{ number(ipcStatus?.heartbeat_ms) }}</strong></span>
            <span>tx_ring_used <strong>{{ number(ipcStatus?.tx_ring_used) }}</strong></span>
            <span>rx_ring_used <strong>{{ number(ipcStatus?.rx_ring_used) }}</strong></span>
            <span>timeout_count <strong>{{ number(ipcStatus?.timeout_count) }}</strong></span>
          </div>
        </article>
      </section>

      <section v-else-if="activeView === 'events'" class="view-stack">
        <div class="section-heading">
          <h2>异常事件</h2>
          <StatusPill :value="events?.parse_error_count ? 'warn' : 'ok'" />
        </div>
        <div v-if="events?.events.length" class="event-list">
          <div v-for="event in events.events" :key="`${event.timestamp_ms}-${event.source}-${event.message}`" class="event-card">
            <StatusPill :value="event.level" />
            <div>
              <strong>{{ event.message || 'no message' }}</strong>
              <span>{{ event.source }} · {{ ms(event.timestamp_ms) }}</span>
              <p>{{ event.detail || 'no detail' }}</p>
            </div>
          </div>
        </div>
        <p v-else class="empty-text">no data</p>
      </section>

      <section v-else class="view-stack">
        <div class="log-toolbar">
          <label>
            来源
            <select v-model="logQuery.source" @change="reloadLogs">
              <option value="linux_app">linux_app</option>
              <option value="web">web</option>
              <option value="system">system</option>
              <option value="can">can</option>
            </select>
          </label>
          <label>
            等级
            <select v-model="logQuery.level" @change="reloadLogs">
              <option value="">all</option>
              <option value="error">error</option>
              <option value="warn">warn</option>
              <option value="info">info</option>
              <option value="debug">debug</option>
            </select>
          </label>
          <label>
            关键字
            <input v-model.trim="logQuery.keyword" type="search" @keyup.enter="reloadLogs" />
          </label>
          <button class="icon-button" type="button" title="刷新日志" @click="reloadLogs">
            <RefreshCw :size="18" />
          </button>
        </div>

        <div v-if="logs?.lines.length" class="log-view">
          <div v-for="line in logs.lines" :key="line.line_number" class="log-line">
            <span>{{ line.line_number }}</span>
            <StatusPill :value="line.level" />
            <code>{{ line.text }}</code>
          </div>
        </div>
        <p v-else class="empty-text">no data</p>
        <button v-if="logs?.has_more" class="load-button" type="button" @click="loadMoreLogs">
          <ChevronDown :size="18" />
          <span>更多</span>
        </button>
      </section>

      <p v-if="lastError" class="error-banner">{{ lastError }}</p>
    </section>
  </main>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import {
  Activity,
  AlertTriangle,
  ChevronDown,
  Clock3,
  Cpu,
  FileText,
  Gauge,
  LayoutDashboard,
  MemoryStick,
  RadioTower,
  RefreshCw,
  Rows3,
  Waypoints
} from 'lucide-vue-next'
import StatusPill from './components/StatusPill.vue'
import {
  getCanStatus,
  getEvents,
  getHealth,
  getIpcStatus,
  getLogs,
  getModules,
  getResources,
  type CanStatusResponse,
  type EventsResponse,
  type HealthResponse,
  type IpcStatusResponse,
  type LogsResponse,
  type ModulesResponse,
  type ResourcesResponse
} from './api/client'

type ViewKey = 'dashboard' | 'modules' | 'resources' | 'bus' | 'events' | 'logs'

const activeView = ref<ViewKey>('dashboard')
const health = ref<HealthResponse | null>(null)
const modules = ref<ModulesResponse | null>(null)
const resources = ref<ResourcesResponse | null>(null)
const canStatus = ref<CanStatusResponse | null>(null)
const ipcStatus = ref<IpcStatusResponse | null>(null)
const events = ref<EventsResponse | null>(null)
const logs = ref<LogsResponse | null>(null)
const lastError = ref('')
const timers: number[] = []

const logQuery = reactive({
  source: 'linux_app',
  level: '',
  keyword: '',
  cursor: '',
  limit: 200
})

const navItems = [
  { key: 'dashboard' as const, label: '总览', icon: LayoutDashboard },
  { key: 'modules' as const, label: '模块', icon: Rows3 },
  { key: 'resources' as const, label: '资源', icon: Gauge },
  { key: 'bus' as const, label: 'CAN / IPC', icon: RadioTower },
  { key: 'events' as const, label: '事件', icon: AlertTriangle },
  { key: 'logs' as const, label: '日志', icon: FileText }
]

const currentTitle = computed(() => navItems.find((item) => item.key === activeView.value)?.label || '总览')
const statusLine = computed(() => {
  const service = health.value?.status || 'unknown'
  const moduleState = modules.value?.state || 'unknown'
  const can = canStatus.value?.state || 'unknown'
  return `service ${service} · modules ${moduleState} · can ${can}`
})

async function run(label: string, task: () => Promise<void>) {
  try {
    await task()
    lastError.value = ''
  } catch (error) {
    lastError.value = `${label}: ${error instanceof Error ? error.message : String(error)}`
  }
}

function refreshAll() {
  void run('health', async () => { health.value = await getHealth() })
  void run('modules', async () => { modules.value = await getModules() })
  void run('resources', async () => { resources.value = await getResources() })
  void run('can-status', async () => { canStatus.value = await getCanStatus() })
  void run('ipc-status', async () => { ipcStatus.value = await getIpcStatus() })
  void run('events', async () => { events.value = await getEvents(50) })
  void reloadLogs()
}

async function reloadLogs() {
  logQuery.cursor = ''
  await run('logs', async () => { logs.value = await getLogs(logQuery) })
}

async function loadMoreLogs() {
  if (!logs.value?.next_cursor) return
  const cursor = logs.value.next_cursor
  await run('logs', async () => {
    const next = await getLogs({ ...logQuery, cursor })
    logs.value = {
      ...next,
      lines: [...(logs.value?.lines || []), ...next.lines]
    }
  })
}

onMounted(() => {
  refreshAll()
  timers.push(window.setInterval(() => run('health', async () => { health.value = await getHealth() }), 5000))
  timers.push(window.setInterval(() => run('modules', async () => { modules.value = await getModules() }), 1000))
  timers.push(window.setInterval(() => run('resources', async () => { resources.value = await getResources() }), 2000))
  timers.push(window.setInterval(() => run('can-status', async () => { canStatus.value = await getCanStatus() }), 1000))
  timers.push(window.setInterval(() => run('ipc-status', async () => { ipcStatus.value = await getIpcStatus() }), 1000))
  timers.push(window.setInterval(() => run('events', async () => { events.value = await getEvents(50) }), 3000))
  timers.push(window.setInterval(() => reloadLogs(), 5000))
})

onBeforeUnmount(() => {
  timers.forEach((timer) => window.clearInterval(timer))
})

function moduleLabel(name: string) {
  const labels: Record<string, string> = {
    four_g: '4G',
    '4g': '4G',
    wifi: 'WiFi',
    bluetooth: 'Bluetooth',
    ethernet: 'Ethernet',
    rs485: 'RS485'
  }
  return labels[name] || name
}

function number(value?: number | null) {
  if (value === undefined || value === null) return '0'
  return new Intl.NumberFormat('zh-CN').format(value)
}

function percent(value?: number | null) {
  if (value === undefined || value === null) return 'unknown'
  return `${value.toFixed(2)}%`
}

function bytes(value?: number | null) {
  if (value === undefined || value === null) return 'unknown'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let size = value
  let idx = 0
  while (size >= 1024 && idx < units.length - 1) {
    size /= 1024
    idx += 1
  }
  return `${size.toFixed(idx === 0 ? 0 : 1)} ${units[idx]}`
}

function bytesFromKb(value?: number | null) {
  if (value === undefined || value === null) return 'unknown'
  return bytes(value * 1024)
}

function duration(seconds?: number | null) {
  if (seconds === undefined || seconds === null) return 'unknown'
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  if (days > 0) return `${days}d ${hours}h`
  if (hours > 0) return `${hours}h ${minutes}m`
  return `${minutes}m`
}

function ms(value?: number | null) {
  if (!value) return 'unknown'
  return `${number(value)} ms`
}
</script>
