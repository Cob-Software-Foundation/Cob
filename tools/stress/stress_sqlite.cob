_MakeCache = False
set h = sql_open("/tmp/stress.db")
set rc = sql_exec(h, "DROP TABLE IF EXISTS s")
set rc = sql_exec(h, "CREATE TABLE s (n INTEGER)")
set i = 0
while i < 2000:
    set rc = sql_exec(h, "INSERT INTO s (n) VALUES (" + i + ")")
    set i = i + 1
set total = sql_query(h, "SELECT COUNT(*) FROM s")
pop("sqlite stress: row count=" + total)
set rc = sql_close(h)
