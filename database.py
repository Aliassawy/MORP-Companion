import sqlite3

DB_NAME = "morp_memory.db"

def init_db():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    
    # Stores dynamic system prompt
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS persona (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE,
            system_prompt TEXT,
            active INTEGER DEFAULT 0
        )
    ''')
    
    # Stores long-term user facts
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS facts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            category TEXT,
            fact_text TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Insert default prompt if table is empty
    cursor.execute("SELECT COUNT(*) FROM persona")
    if cursor.fetchone()[0] == 0:
        default_prompt = (
            "You are MORP, a witty, snarky desktop sidekick bot. "
            "Give short, sarcastic, yet helpful responses. "
            "When learning new user details, save them using tags like: [SAVE_FACT: category | fact]."
        )
        cursor.execute("INSERT INTO persona (name, system_prompt, active) VALUES ('default', ?, 1)", (default_prompt,))
    
    conn.commit()
    conn.close()

def get_active_prompt() -> str:
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute("SELECT system_prompt FROM persona WHERE active = 1 LIMIT 1")
    row = cursor.fetchone()
    conn.close()
    return row[0] if row else "You are MORP, a witty desktop companion."

def update_persona(new_prompt: str):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute("UPDATE persona SET system_prompt = ? WHERE active = 1", (new_prompt,))
    conn.commit()
    conn.close()

def save_fact(category: str, fact_text: str):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute("INSERT INTO facts (category, fact_text) VALUES (?, ?)", (category, fact_text))
    conn.commit()
    conn.close()

def get_all_facts() -> str:
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()
    cursor.execute("SELECT category, fact_text FROM facts")
    rows = cursor.fetchall()
    conn.close()
    if not rows:
        return "No learned facts yet."
    return "\n".join([f"- [{cat}]: {text}" for cat, text in rows])

if __name__ == "__main__":
    init_db()
    print("SQLite Database initialized.")