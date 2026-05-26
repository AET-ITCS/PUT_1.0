export interface HealthResponse {
  service: string
  status: string
  readonly: boolean
  version: string
  architecture: string
}

export interface ModuleStatus {
  name: string
  status: string
  rx_bytes: number
  tx_bytes: number
  rx_frames: number
  tx_frames: number
  decode_error_count: number
  fragment_drop_count: number
  reassemble_timeout_count: number
  crc_error_count: number
  send_fail_count: number
  interface_offline_count: number
  last_rx_ms: number
  last_tx_ms: number
  last_error: string
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

export interface DeviceNodeInfo {
  key: string
  label: string
  state: string
  present: boolean | null
  checked_paths: string[]
  matched_paths: string[]
}

export interface ResourcesResponse {
  cpu: CpuInfo
  memory: MemoryInfo
  uptime: UptimeInfo
  disks: DiskInfo[]
  networks: NetworkInfo[]
  devices: DeviceNodeInfo[]
}

export interface FramePoolStatus {
  capacity: number
  used: number
  high_watermark: number
  full_count: number
  allocated: number
  released: number
  pending_reclaim: number
  leaked_suspect: number
}

export interface RingStatus {
  interface: string
  capacity: number
  used: number
  high_watermark: number
  full_count: number
}

export interface PendingBitmapStatus {
  rx: string
  tx: string
}

export interface MailboxStatus {
  rx_doorbell_count: number
  tx_doorbell_count: number
  notify_fail_count: number
  periodic_drain_count: number
}

export interface IntegrityStatus {
  descriptor_crc_error_count: number
  epoch_mismatch_count: number
  cache_sync_error_count: number
}

export interface ReclaimStatus {
  heartbeat_consumed: number
  invalid_frame_reclaimed: number
  no_route_reclaimed: number
  ttl_expired_reclaimed: number
  epoch_mismatch_reclaimed: number
  reclaim_ring_used: number
  reclaim_ack_count: number
}

export interface IpcStatusResponse {
  updated_at_ms: number
  state: string
  rtos_online: boolean
  heartbeat_ms: number
  frame_pool: FramePoolStatus
  rx_rings: RingStatus[]
  tx_rings: RingStatus[]
  pending_bitmap: PendingBitmapStatus
  mailbox: MailboxStatus
  integrity: IntegrityStatus
  reclaim: ReclaimStatus
}

export interface RouteTableStatus {
  version: number
  epoch: number
  source: string
  active_entries: number
}

export interface PriorityQueueStatus {
  priority: number
  queued: number
  capacity: number
  routed_frames: number
  dropped_frames: number
  max_latency_ms: number
}

export interface CidStats {
  routed_frames: number
  heartbeat_consumed: number
  no_route: number
  invalid_cid: number
  reserved_cid: number
  broadcast_frames: number
}

export interface DropReasons {
  invalid_length: number
  invalid_type: number
  ttl_expired: number
  frame_pool_full: number
  rx_ring_full: number
  tx_ring_full: number
  target_interface_offline: number
  auth_failed: number
  integrity_failed: number
  replay_dropped: number
}

export interface LatencyStats {
  rx_ring_to_tx_ring_max_ms: number
  rx_ring_to_tx_ring_avg_ms: number
  linux_egress_max_ms: number
  end_to_end_max_ms: number
}

export interface RouteStatusResponse {
  updated_at_ms: number
  state: string
  route_table: RouteTableStatus
  priority_queues: PriorityQueueStatus[]
  cid_stats: CidStats
  drop_reasons: DropReasons
  latency: LatencyStats
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

export function getIpcStatus() {
  return apiGet<IpcStatusResponse>('/api/ipc-status')
}

export function getRouteStatus() {
  return apiGet<RouteStatusResponse>('/api/route-status')
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
