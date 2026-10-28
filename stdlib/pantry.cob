# pantry -- a SQLite database to keep things in
#   pantry_open(path)  pantry_exec(h, sql)  pantry_query(h, sql)  pantry_close(h)
extern pantry_open(path) from "pantry"
extern pantry_exec(h, sql) from "pantry"
extern pantry_query(h, sql) from "pantry"
extern pantry_close(h) from "pantry"
