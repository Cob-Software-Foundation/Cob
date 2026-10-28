# pantry: SQLite as a shared library
shuck pantry
shuck granary

set path = "pantry_test_tmp.db"
granary_remove(path)

set db = pantry_open(path)
if db > 0:
    pop("opened")
pop(pantry_exec(db, "CREATE TABLE crops (name TEXT, bushels INTEGER)"))
pop(pantry_exec(db, "INSERT INTO crops VALUES ('corn', 40), ('wheat', 25), ('oats', 7)"))
pop(pantry_query(db, "SELECT COUNT(*) FROM crops"))
pop(pantry_query(db, "SELECT name FROM crops ORDER BY bushels DESC"))
pop(pantry_query(db, "SELECT SUM(bushels) FROM crops"))
pop("[" + pantry_query(db, "SELECT name FROM crops WHERE bushels > 1000") + "]")
pop(pantry_exec(db, "THIS IS NOT SQL"))
pop(pantry_close(db))
pop(pantry_exec(db, "SELECT 1"))
granary_remove(path)
