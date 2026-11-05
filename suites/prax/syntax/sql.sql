-- The schema of a small shop, and a report of each customer's spending this year.
CREATE TABLE customers (
    id          INTEGER PRIMARY KEY,
    email       VARCHAR(255) NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE orders (
    id           INTEGER PRIMARY KEY,
    customer_id  INTEGER NOT NULL REFERENCES customers (id) ON DELETE CASCADE,
    total_cents  INTEGER NOT NULL CHECK (total_cents >= 0),
    status       VARCHAR(16) NOT NULL DEFAULT 'pending',
    placed_at    TIMESTAMP NOT NULL
);

CREATE INDEX orders_by_customer ON orders (customer_id, placed_at);

/* Customers with at least one paid order this year, by what they spent. */
WITH paid AS (
    SELECT customer_id, SUM(total_cents) AS spent, COUNT(*) AS orders
    FROM orders
    WHERE status = 'paid'
      AND placed_at >= '2026-01-01'
    GROUP BY customer_id
)
SELECT
    c.name,
    c.email,
    p.orders,
    ROUND(p.spent / 100.0, 2) AS spent,
    CASE
        WHEN p.spent > 100000 THEN 'gold'
        WHEN p.spent > 25000 THEN 'silver'
        ELSE 'bronze'
    END AS tier
FROM customers AS c
JOIN paid AS p ON p.customer_id = c.id
WHERE c.email NOT LIKE '%@example.com'
ORDER BY p.spent DESC
LIMIT 20;

UPDATE orders SET status = 'cancelled' WHERE status = 'pending' AND placed_at < '2026-01-01';
