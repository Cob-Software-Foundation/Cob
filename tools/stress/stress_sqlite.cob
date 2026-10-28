_MakeCache = False
shuck pantry
set h = pantry_open("/tmp/stress.db")
set rc = pantry_exec(h, "DROP TABLE IF EXISTS s")
set rc = pantry_exec(h, "CREATE TABLE s (n INTEGER)")
set i = 0
while i < 2000:
    set rc = pantry_exec(h, "INSERT INTO s (n) VALUES (" + i + ")")
    set i = i + 1
set total = pantry_query(h, "SELECT COUNT(*) FROM s")
pop("sqlite stress: row count=" + total)
set rc = pantry_close(h)
