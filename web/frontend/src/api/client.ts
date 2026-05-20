export interface HealthResponse {
  service: string
  status: string
  readonly: boolean
  version: string
}

export interface ModuleStatus {
  name: string
  status: string
  rx_count: number
  tx_count: number
  error_count: number
  last_seen_ms: number
  message: string
}

export interface ModulesResponse {
  updated_at_ms: number
  state: string
  modules: ModuleStatus[]
}

export interface CpuInfo {
  state: string
  usage_percent: number | null
}

export interface MemoryInfo {
  state: string
  total_kb: number | null
  available_kb: number | null
  used_kb: number | null
  usage_percent: number | null
}

export interface UptimeInfo {
  state: string
  uptime_seconds: number | null
}

export interface DiskInfo {
  mount_point: string
  filesystem: string
  total_bytes: number
  available_bytes: number
  used_bytes: number
  usage_percent: number
}

export interface NetworkInfo {
  name: string
  state: string
  rx_bytes: number
  tx_bytes: number
}

export interface ResourcesResponse {
  cpu: CpuInfo
  memory: MemoryInfo
  uptime: UptimeInfo
  disks: DiskInfo[]
  networks: NetworkInfo[]
}

export interface CanStatusResponse {
  updated_at_ms: number
  state: string
  bus_state: string
  tx_count: number
  rx_count: number
  error_count: number
  drop_count: number
  last_error: string
}

export interface IpcStatusResponse {
  updated_at_ms: number
  state: string
  online: boolean
  heartbeat_ms: number
  tx_ring_used: number
  rx_ring_used: number
  timeout_count: number
}

export interface EventRecord {
  timestamp_ms: number
  level: string
  source: string
  message: string
  detail: string
}

export interface EventsResponse {
  events: EventRecord[]
  parse_error_count: number
}

export interface LogLine {
  line_number: number
  source: string
  level: string
  text: string
}

export interface LogsResponse {
  source: string
  lines: LogLine[]
  next_cursor: string | null
  has_more: boolean
}

export interface LogsQuery {
  source: string
  level?: string
  keyword?: string
  cursor?: string
  limit?: number
}

async function apiGet<T>(path: string): Promise<T> {
  const response = await fetch(path, {
    headers: {
      Accept: 'application/json'
    }
  })
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`)
  }
  return response.json() as Promise<T>
}

export function getHealth() {
  return apiGet<HealthResponse>('/api/health')
}

export function getModules() {
  return apiGet<ModulesResponse>('/api/modules')
}

export function getResources() {
  return apiGet<ResourcesResponse>('/api/resources')
}

export function getCanStatus() {
  return apiGet<CanStatusResponse>('/api/can-status')
}

export function getIpcStatus() {
  return apiGet<IpcStatusResponse>('/api/ipc-status')
}

export function getEvents(limit = 50) {
  return apiGet<EventsResponse>(`/api/events?limit=${limit}`)
}

export function getLogs(query: LogsQuery) {
  const params = new URLSearchParams()
  params.set('source', query.source || 'linux_app')
  if (query.level) params.set('level', query.level)
  if (query.keyword) params.set('keyword', query.keyword)
  if (query.cursor) params.set('cursor', query.cursor)
  params.set('limit', String(query.limit ?? 200))
  return apiGet<LogsResponse>(`/api/logs?${params.toString()}`)
}

