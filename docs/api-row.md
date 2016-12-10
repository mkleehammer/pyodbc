# Row

Row objects are returned from Cursor fetch functions.  As specified in the DB API, they are
tuple-like.

    row = cursor.fetchone()
    print(row[0])

However, there are some pyodbc additions that make them very convenient:

* Values can be accessed by column name.
* The Cursor.description values can be accessed from the row.
* Values can be replaced
* Rows from the same select share memory.

Accessing rows by column name is very convenient, readable, and Pythonish:

    cursor.execute("select album_id, photo_id from photos where user_id=1")
    row = cursor.fetchone()
    print(row.album_id, row.photo_id)
    print(row[0], row[1]) # same as above, but less readable

Having access to the cursor's description, even after the curosr is closed, makes Rows very
convenient data structures -- you can pass them around and they are self describing:

    def getuser(userid):
        cnxn = pyodbc.connect(...)
        cursor = cnxn.cursor()
        return cursor.execute(
            """
            select album_id, photo_id
            from photos
            where user_id = ?
            """, userid).fetchall()

    row = getuser(7)
    # At this point the cursor has been closed and deleted
    # But the columns and datatypes can still be accessed:
    print('columns:', ', '.join(t[0] for t in row.cursor_description))

Unlike normal tuples, values in Row objects can be replaced.  (This means you shouldn't use
rows as dictionary keys!)

The intention is to make Rows convenient data structures to replace small or short-lived
classes.  While SQL is powerful, there are sometimes slight changes that need to be made after
reading values:

    # Replace the 'start_date' datetime in each row with one that has a time zone.
    rows = cursor.fetchall()
    for row in rows:
    row.start_date = row.start_date.astimezone(tz)

Note that slicing rows returns tuples, not Row objects!

## Variables

### cursor_description

A copy of the Cursor.description object from the Cursor that created this row.  This contains
the column names and data types of the columns.
