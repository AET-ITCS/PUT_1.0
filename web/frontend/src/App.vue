<template>
  <main class="app-shell">
    <aside class="sidebar">
      <div class="brand">
        <Activity class="brand-icon" :size="26" />
        <div>
          <strong>PUT Monitor</strong>
          <span>{{ health?.version || '0.2.1' }}</span>
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
            <span>物理接口</span>
          </div>
          <strong>{{ interfaceSummary }}</strong>
          <small>can / ethernet / wifi / bluetooth / 4g / rs485</small>
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <Waypoints :size="18" />
            <span>小核</span>
          </div>
          <strong>{{ ipcStatus?.rtos_online ? 'online' : 'offline' }}</strong>
          <StatusPill :value="ipcStatus?.state" />
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <Gauge :size="18" />
            <span>Frame Pool</span>
          </div>
          <strong>{{ poolUsageText }}</strong>
          <small>{{ number(ipcStatus?.frame_pool.used) }} / {{ number(ipcStatus?.frame_pool.capacity) }}</small>
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <Rows3 :size="18" />
            <span>Ring 水位</span>
          </div>
          <strong>{{ ringWatermarkText }}</strong>
          <small>RX/TX descriptor high watermark</small>
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <AlertTriangle :size="18" />
            <span>安全异常</span>
          </div>
          <strong>{{ number(securityDropTotal) }}</strong>
          <StatusPill :value="securityDropTotal ? 'error' : 'ok'" />
        </article>
        <article class="metric-panel">
          <div class="panel-title">
            <Clock3 :size="18" />
            <span>端到端延迟</span>
          </div>
          <strong>{{ ms(routeStatus?.latency.end_to_end_max_ms) }}</strong>
          <StatusPill :value="routeStatus?.state" />
        </article>

        <article class="wide-panel">
          <div class="section-heading">
            <h2>六类接口状态</h2>
            <StatusPill :value="modules?.state" />
          </div>
          <div v-if="modules?.modules.length" class="module-strip">
            <div v-for="module in modules.modules" :key="module.name" class="module-tile">
              <div>
                <strong>{{ moduleLabel(module.name) }}</strong>
                <span>{{ bytes(module.rx_bytes) }} RX · {{ bytes(module.tx_bytes) }} TX</span>
                <span>{{ module.message || module.last_error || 'no message' }}</span>
              </div>
              <StatusPill :value="module.status" />
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>

        <article class="wide-panel">
          <div class="section-heading">
            <h2>最近严重异常</h2>
            <StatusPill :value="seriousItems.length ? 'warn' : 'ok'" />
          </div>
          <div v-if="seriousItems.length" class="event-list compact">
            <div
              v-for="item in seriousItems"
              :key="`${item.source}-${item.message}-${item.timestamp_ms || 0}`"
              class="event-row"
              :class="{ security: isSecurityText(item.message + item.detail) }"
            >
              <StatusPill :value="item.level" />
              <span>{{ item.source }}</span>
              <strong>{{ item.message }}</strong>
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>
      </section>

      <section v-else-if="activeView === 'modules'" class="view-stack">
        <div class="section-heading">
          <h2>物理接口</h2>
          <StatusPill :value="modules?.state" />
        </div>
        <div v-if="modules?.modules.length" class="data-table module-table">
          <div class="table-row table-head">
            <span>接口</span><span>状态</span><span>RX 字节</span><span>TX 字节</span><span>RX 帧</span><span>TX 帧</span><span>分片/重组</span><span>CRC/发送</span><span>最近收发</span><span>最近错误</span>
          </div>
          <div v-for="module in modules.modules" :key="module.name" class="table-row">
            <strong>{{ moduleLabel(module.name) }}</strong>
            <StatusPill :value="module.status" />
            <span>{{ bytes(module.rx_bytes) }}</span>
            <span>{{ bytes(module.tx_bytes) }}</span>
            <span>{{ number(module.rx_frames) }}</span>
            <span>{{ number(module.tx_frames) }}</span>
            <span>{{ number(module.fragment_drop_count) }} / {{ number(module.reassemble_timeout_count) }}</span>
            <span>{{ number(module.crc_error_count) }} / {{ number(module.send_fail_count) }}</span>
            <span>{{ lastIoText(module) }}</span>
            <span>{{ module.last_error || 'none' }}</span>
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

      <section v-else-if="activeView === 'ipcRoute'" class="view-stack">
        <div class="summary-grid">
          <article class="metric-panel">
            <div class="panel-title"><Waypoints :size="18" /><span>IPC</span></div>
            <strong>{{ ipcStatus?.rtos_online ? 'online' : 'offline' }}</strong>
            <StatusPill :value="ipcStatus?.state" />
          </article>
          <article class="metric-panel">
            <div class="panel-title"><Gauge :size="18" /><span>Frame Pool</span></div>
            <strong>{{ poolUsageText }}</strong>
            <small>full {{ number(ipcStatus?.frame_pool.full_count) }} · pending {{ number(ipcStatus?.frame_pool.pending_reclaim) }}</small>
          </article>
          <article class="metric-panel">
            <div class="panel-title"><Rows3 :size="18" /><span>Route</span></div>
            <strong>v{{ number(routeStatus?.route_table.version) }}</strong>
            <StatusPill :value="routeStatus?.state" />
          </article>
        </div>

        <article class="wide-panel">
          <div class="section-heading">
            <h2>Frame Pool</h2>
            <StatusPill :value="poolUsageTone" />
          </div>
          <div class="stat-list">
            <span v-for="item in framePoolItems" :key="item.label">{{ item.label }} <strong>{{ item.value }}</strong></span>
          </div>
        </article>

        <article class="wide-panel">
          <div class="section-heading"><h2>Descriptor Ring</h2></div>
          <div v-if="ringRows.length" class="data-table ring-table">
            <div class="table-row table-head">
              <span>方向</span><span>接口</span><span>占用</span><span>容量</span><span>水位</span><span>满计数</span>
            </div>
            <div v-for="ring in ringRows" :key="`${ring.direction}-${ring.interface}`" class="table-row">
              <strong>{{ ring.direction }}</strong>
              <span>{{ moduleLabel(ring.interface) }}</span>
              <span>{{ number(ring.used) }}</span>
              <span>{{ number(ring.capacity) }}</span>
              <span>{{ number(ring.high_watermark) }}</span>
              <span>{{ number(ring.full_count) }}</span>
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>

        <article class="wide-panel">
          <div class="section-heading"><h2>Mailbox / 完整性</h2></div>
          <div class="stat-list">
            <span>rx pending <strong>{{ ipcStatus?.pending_bitmap.rx || '0x00' }}</strong></span>
            <span>tx pending <strong>{{ ipcStatus?.pending_bitmap.tx || '0x00' }}</strong></span>
            <span>rx doorbell <strong>{{ number(ipcStatus?.mailbox.rx_doorbell_count) }}</strong></span>
            <span>tx doorbell <strong>{{ number(ipcStatus?.mailbox.tx_doorbell_count) }}</strong></span>
            <span>notify fail <strong>{{ number(ipcStatus?.mailbox.notify_fail_count) }}</strong></span>
            <span>periodic drain <strong>{{ number(ipcStatus?.mailbox.periodic_drain_count) }}</strong></span>
            <span>descriptor crc <strong>{{ number(ipcStatus?.integrity.descriptor_crc_error_count) }}</strong></span>
            <span>epoch mismatch <strong>{{ number(ipcStatus?.integrity.epoch_mismatch_count) }}</strong></span>
            <span>cache sync <strong>{{ number(ipcStatus?.integrity.cache_sync_error_count) }}</strong></span>
          </div>
        </article>

        <article class="wide-panel">
          <div class="section-heading"><h2>回收闭环</h2></div>
          <div class="stat-list">
            <span v-for="item in reclaimItems" :key="item.label">{{ item.label }} <strong>{{ item.value }}</strong></span>
          </div>
        </article>

        <article class="wide-panel">
          <div class="section-heading"><h2>Route Table</h2></div>
          <div class="stat-list">
            <span>version <strong>{{ number(routeStatus?.route_table.version) }}</strong></span>
            <span>epoch <strong>{{ number(routeStatus?.route_table.epoch) }}</strong></span>
            <span>source <strong>{{ routeStatus?.route_table.source || 'unknown' }}</strong></span>
            <span>active entries <strong>{{ number(routeStatus?.route_table.active_entries) }}</strong></span>
          </div>
        </article>

        <article class="wide-panel">
          <div class="section-heading"><h2>Priority Queues</h2></div>
          <div v-if="routeStatus?.priority_queues.length" class="data-table queue-table">
            <div class="table-row table-head">
              <span>priority</span><span>queued</span><span>capacity</span><span>routed</span><span>dropped</span><span>max latency</span>
            </div>
            <div v-for="queue in routeStatus.priority_queues" :key="queue.priority" class="table-row">
              <strong>{{ queue.priority }}</strong>
              <span>{{ number(queue.queued) }}</span>
              <span>{{ number(queue.capacity) }}</span>
              <span>{{ number(queue.routed_frames) }}</span>
              <span>{{ number(queue.dropped_frames) }}</span>
              <span>{{ ms(queue.max_latency_ms) }}</span>
            </div>
          </div>
          <p v-else class="empty-text">no data</p>
        </article>

        <div class="detail-grid">
          <article class="wide-panel">
            <div class="section-heading"><h2>CID Stats</h2></div>
            <div class="stat-list">
              <span v-for="item in cidStatItems" :key="item.label">{{ item.label }} <strong>{{ item.value }}</strong></span>
            </div>
          </article>

          <article class="wide-panel">
            <div class="section-heading">
              <h2>Drop Reasons</h2>
              <StatusPill :value="securityDropTotal ? 'error' : routeStatus?.state" />
            </div>
            <div class="stat-list">
              <span
                v-for="item in dropReasonItems"
                :key="item.label"
                :class="{ security: isSecurityName(item.key) && item.raw > 0 }"
              >
                {{ item.label }} <strong>{{ item.value }}</strong>
              </span>
            </div>
          </article>

          <article class="wide-panel">
            <div class="section-heading"><h2>Latency</h2></div>
            <div class="stat-list">
              <span v-for="item in latencyItems" :key="item.label">{{ item.label }} <strong>{{ item.value }}</strong></span>
            </div>
          </article>
        </div>
      </section>

      <section v-else-if="activeView === 'events'" class="view-stack">
        <div class="section-heading">
          <h2>异常事件</h2>
          <StatusPill :value="events?.parse_error_count ? 'warn' : 'ok'" />
        </div>
        <div v-if="events?.events.length" class="event-list">
          <div
            v-for="event in events.events"
            :key="`${event.timestamp_ms}-${event.source}-${event.message}`"
            class="event-card"
            :class="{ security: isSecurityEvent(event) }"
          >
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
              <option v-for="source in logSources" :key="source" :value="source">{{ source }}</option>
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
  getEvents,
  getHealth,
  getIpcStatus,
  getLogs,
  getModules,
  getResources,
  getRouteStatus,
  type EventRecord,
  type EventsResponse,
  type HealthResponse,
  type IpcStatusResponse,
  type LogsResponse,
  type ModuleStatus,
  type ModulesResponse,
  type ResourcesResponse,
  type RingStatus,
  type RouteStatusResponse
} from './api/client'

type ViewKey = 'dashboard' | 'modules' | 'resources' | 'ipcRoute' | 'events' | 'logs'

interface AlertItem {
  level: string
  source: string
  message: string
  detail: string
  timestamp_ms?: number
}

interface RingRow extends RingStatus {
  direction: 'RX' | 'TX'
}

const activeView = ref<ViewKey>('dashboard')
const health = ref<HealthResponse | null>(null)
const modules = ref<ModulesResponse | null>(null)
const resources = ref<ResourcesResponse | null>(null)
const ipcStatus = ref<IpcStatusResponse | null>(null)
const routeStatus = ref<RouteStatusResponse | null>(null)
const events = ref<EventsResponse | null>(null)
const logs = ref<LogsResponse | null>(null)
const lastError = ref('')
const timers: number[] = []
const logSources = ['linux_app', 'web', 'system', 'ipc', 'router', 'adapter']

const logQuery = reactive({
  source: 'linux_app',
  level: '',
  keyword: '',
  cursor: '',
  limit: 200
})

const navItems = [
  { key: 'dashboard' as const, label: '总览', icon: LayoutDashboard },
  { key: 'modules' as const, label: '接口', icon: RadioTower },
  { key: 'resources' as const, label: '资源', icon: Gauge },
  { key: 'ipcRoute' as const, label: 'IPC / 路由', icon: Waypoints },
  { key: 'events' as const, label: '事件', icon: AlertTriangle },
  { key: 'logs' as const, label: '日志', icon: FileText }
]

const currentTitle = computed(() => navItems.find((item) => item.key === activeView.value)?.label || '总览')
const statusLine = computed(() => {
  const service = health.value?.status || 'unknown'
  const moduleState = modules.value?.state || 'unknown'
  const ipc = ipcStatus.value?.state || 'unknown'
  const route = routeStatus.value?.state || 'unknown'
  return `service ${service} · modules ${moduleState} · ipc ${ipc} · route ${route}`
})

const interfaceSummary = computed(() => {
  const items = modules.value?.modules || []
  if (!items.length) return 'unknown'
  const online = items.filter((item) => item.status === 'online' || item.status === 'ok').length
  return `${online} / ${items.length}`
})

const poolUsage = computed(() => ratioPercent(ipcStatus.value?.frame_pool.used, ipcStatus.value?.frame_pool.capacity))
const poolUsageText = computed(() => percent(poolUsage.value))
const poolUsageTone = computed(() => {
  if (poolUsage.value === null) return 'unknown'
  if (poolUsage.value >= 90) return 'error'
  if (poolUsage.value >= 75) return 'warn'
  return 'ok'
})

const ringRows = computed<RingRow[]>(() => [
  ...(ipcStatus.value?.rx_rings || []).map((ring) => ({ ...ring, direction: 'RX' as const })),
  ...(ipcStatus.value?.tx_rings || []).map((ring) => ({ ...ring, direction: 'TX' as const }))
])

const ringWatermarkText = computed(() => {
  const ratios = ringRows.value
    .map((ring) => ratioPercent(ring.high_watermark || ring.used, ring.capacity))
    .filter((value): value is number => value !== null)
  if (!ratios.length) return 'unknown'
  return percent(Math.max(...ratios))
})

const securityDropTotal = computed(() => {
  const drops = routeStatus.value?.drop_reasons
  return (drops?.auth_failed || 0) + (drops?.integrity_failed || 0) + (drops?.replay_dropped || 0)
})

const seriousItems = computed<AlertItem[]>(() => {
  const recentEvents: AlertItem[] = (events.value?.events || [])
    .filter((event) => ['warn', 'error'].includes(event.level) || isSecurityEvent(event))
    .slice(-5)
    .map((event) => ({
      level: event.level,
      source: event.source,
      message: event.message,
      detail: event.detail,
      timestamp_ms: event.timestamp_ms
    }))

  if (securityDropTotal.value > 0) {
    recentEvents.push({
      level: 'error',
      source: 'router',
      message: 'security drops',
      detail: securityDropSummary.value
    })
  }

  return recentEvents.slice(-5)
})

const securityDropSummary = computed(() => {
  const drops = routeStatus.value?.drop_reasons
  return `auth_failed=${number(drops?.auth_failed)} integrity_failed=${number(drops?.integrity_failed)} replay_dropped=${number(drops?.replay_dropped)}`
})

const framePoolItems = computed(() => {
  const pool = ipcStatus.value?.frame_pool
  return [
    { label: 'capacity', value: number(pool?.capacity) },
    { label: 'used', value: number(pool?.used) },
    { label: 'high watermark', value: number(pool?.high_watermark) },
    { label: 'full count', value: number(pool?.full_count) },
    { label: 'allocated', value: number(pool?.allocated) },
    { label: 'released', value: number(pool?.released) },
    { label: 'pending reclaim', value: number(pool?.pending_reclaim) },
    { label: 'leaked suspect', value: number(pool?.leaked_suspect) }
  ]
})

const reclaimItems = computed(() => {
  const reclaim = ipcStatus.value?.reclaim
  return [
    { label: 'heartbeat consumed', value: number(reclaim?.heartbeat_consumed) },
    { label: 'invalid frame', value: number(reclaim?.invalid_frame_reclaimed) },
    { label: 'no route', value: number(reclaim?.no_route_reclaimed) },
    { label: 'ttl expired', value: number(reclaim?.ttl_expired_reclaimed) },
    { label: 'epoch mismatch', value: number(reclaim?.epoch_mismatch_reclaimed) },
    { label: 'reclaim ring used', value: number(reclaim?.reclaim_ring_used) },
    { label: 'reclaim ack', value: number(reclaim?.reclaim_ack_count) }
  ]
})

const cidStatItems = computed(() => {
  const stats = routeStatus.value?.cid_stats
  return [
    { label: 'routed frames', value: number(stats?.routed_frames) },
    { label: 'heartbeat consumed', value: number(stats?.heartbeat_consumed) },
    { label: 'no route', value: number(stats?.no_route) },
    { label: 'invalid cid', value: number(stats?.invalid_cid) },
    { label: 'reserved cid', value: number(stats?.reserved_cid) },
    { label: 'broadcast frames', value: number(stats?.broadcast_frames) }
  ]
})

const dropReasonItems = computed(() => {
  const drops = routeStatus.value?.drop_reasons
  return [
    { key: 'invalid_length', label: 'invalid length', raw: drops?.invalid_length || 0, value: number(drops?.invalid_length) },
    { key: 'invalid_type', label: 'invalid type', raw: drops?.invalid_type || 0, value: number(drops?.invalid_type) },
    { key: 'ttl_expired', label: 'ttl expired', raw: drops?.ttl_expired || 0, value: number(drops?.ttl_expired) },
    { key: 'frame_pool_full', label: 'frame pool full', raw: drops?.frame_pool_full || 0, value: number(drops?.frame_pool_full) },
    { key: 'rx_ring_full', label: 'rx ring full', raw: drops?.rx_ring_full || 0, value: number(drops?.rx_ring_full) },
    { key: 'tx_ring_full', label: 'tx ring full', raw: drops?.tx_ring_full || 0, value: number(drops?.tx_ring_full) },
    {
      key: 'target_interface_offline',
      label: 'target offline',
      raw: drops?.target_interface_offline || 0,
      value: number(drops?.target_interface_offline)
    },
    { key: 'auth_failed', label: 'auth failed', raw: drops?.auth_failed || 0, value: number(drops?.auth_failed) },
    {
      key: 'integrity_failed',
      label: 'integrity failed',
      raw: drops?.integrity_failed || 0,
      value: number(drops?.integrity_failed)
    },
    { key: 'replay_dropped', label: 'replay dropped', raw: drops?.replay_dropped || 0, value: number(drops?.replay_dropped) }
  ]
})

const latencyItems = computed(() => {
  const latency = routeStatus.value?.latency
  return [
    { label: 'rx to tx max', value: ms(latency?.rx_ring_to_tx_ring_max_ms) },
    { label: 'rx to tx avg', value: ms(latency?.rx_ring_to_tx_ring_avg_ms) },
    { label: 'linux egress max', value: ms(latency?.linux_egress_max_ms) },
    { label: 'end to end max', value: ms(latency?.end_to_end_max_ms) }
  ]
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
  void run('ipc-status', async () => { ipcStatus.value = await getIpcStatus() })
  void run('route-status', async () => { routeStatus.value = await getRouteStatus() })
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
  timers.push(window.setInterval(() => run('ipc-status', async () => { ipcStatus.value = await getIpcStatus() }), 1000))
  timers.push(window.setInterval(() => run('route-status', async () => { routeStatus.value = await getRouteStatus() }), 1000))
  timers.push(window.setInterval(() => run('events', async () => { events.value = await getEvents(50) }), 3000))
  timers.push(window.setInterval(() => reloadLogs(), 5000))
})

onBeforeUnmount(() => {
  timers.forEach((timer) => window.clearInterval(timer))
})

function moduleLabel(name: string) {
  const labels: Record<string, string> = {
    can: 'CAN',
    four_g: '4G',
    '4g': '4G',
    wifi: 'Wi-Fi',
    bluetooth: 'Bluetooth',
    ethernet: 'Ethernet',
    rs485: 'RS485'
  }
  return labels[name] || name
}

function lastIoText(module: ModuleStatus) {
  return `rx ${ms(module.last_rx_ms)} / tx ${ms(module.last_tx_ms)}`
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

function ratioPercent(used?: number | null, capacity?: number | null) {
  if (!capacity || used === undefined || used === null) return null
  return (used / capacity) * 100
}

function isSecurityName(name: string) {
  return ['auth_failed', 'integrity_failed', 'replay_dropped'].includes(name)
}

function isSecurityText(text: string) {
  const lower = text.toLowerCase()
  return ['auth_failed', 'integrity_failed', 'replay_dropped', 'auth failed', 'integrity failed', 'replay dropped'].some((term) =>
    lower.includes(term)
  )
}

function isSecurityEvent(event: EventRecord) {
  return isSecurityText(`${event.message} ${event.detail}`)
}
</script>
