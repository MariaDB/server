sysbench.cmdline.options = {
  preload_rows = {"Rows inserted during prepare", 2500000},
  measured_rows = {"Rows inserted during run", 400000},
  payload_size = {"Payload bytes per row", 2500},
  batch_size = {"Rows per preload statement", 250}
}

local stride = 1000000
local multiplier = 104729
local shift = 12347

function prepare()
  local driver = sysbench.sql.driver()
  local connection = driver:connect()
  connection:query("DROP TABLE IF EXISTS split_heavy")
  connection:query(string.format([[
    CREATE TABLE split_heavy (
      id BIGINT UNSIGNED NOT NULL,
      pad VARBINARY(%d) NOT NULL,
      PRIMARY KEY(id)
    ) ENGINE=InnoDB ROW_FORMAT=DYNAMIC
  ]], sysbench.opt.payload_size))

  for first = 1, sysbench.opt.preload_rows, sysbench.opt.batch_size do
    local rows = {}
    local last = math.min(first + sysbench.opt.batch_size - 1,
                          sysbench.opt.preload_rows)
    for i = first, last do
      rows[#rows + 1] = string.format("(%.0f,REPEAT('x',%d))",
                                      i * stride,
                                      sysbench.opt.payload_size)
    end
    connection:query("INSERT INTO split_heavy VALUES " ..
                     table.concat(rows, ","))
  end
  connection:disconnect()
end

function cleanup()
  local driver = sysbench.sql.driver()
  local connection = driver:connect()
  connection:query("DROP TABLE IF EXISTS split_heavy")
  connection:disconnect()
end

function thread_init()
  local driver = sysbench.sql.driver()
  connection = driver:connect()
  statement = connection:prepare(
    string.format("INSERT INTO split_heavy VALUES (?,REPEAT('y',%d))",
                  sysbench.opt.payload_size))
  id_param = statement:bind_create(sysbench.sql.type.BIGINT)
  statement:bind_param(id_param)
  local_event = 0
end

function event()
  local total = sysbench.opt.measured_rows
  local slot = local_event * sysbench.opt.threads + sysbench.tid
  local cycle = math.floor(slot / total)
  local logical = slot % total
  local permuted = (logical * multiplier + shift) % total
  local bucket = math.floor(permuted * sysbench.opt.preload_rows / total) + 1
  local id = bucket * stride + 500000 + cycle
  id_param:set(id)
  statement:execute()
  local_event = local_event + 1
end

function thread_done()
  statement:close()
  connection:disconnect()
end
