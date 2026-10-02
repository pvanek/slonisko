-- SPDX-FileCopyrightText: 2026 Petr Vanek
-- SPDX-License-Identifier: GPL-3.0-or-later

-- The sample database the manual's screenshots show: a small shop, with
-- enough keys for a diagram and enough rows for a plan worth reading.

CREATE SCHEMA shop;
SET search_path = shop;
CREATE TABLE category (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL UNIQUE,
    parent_id int REFERENCES category(id)
);
CREATE TABLE product (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    category_id int NOT NULL REFERENCES category(id),
    sku text NOT NULL UNIQUE,
    name text NOT NULL,
    price numeric(10,2) NOT NULL CHECK (price >= 0),
    in_stock int NOT NULL DEFAULT 0
);
CREATE TABLE customer (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    email text NOT NULL UNIQUE,
    full_name text NOT NULL,
    country char(2) NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE "order" (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    customer_id int NOT NULL REFERENCES customer(id),
    ordered_at timestamptz NOT NULL DEFAULT now(),
    status text NOT NULL DEFAULT 'new' CHECK (status IN ('new','paid','shipped','cancelled'))
);
CREATE TABLE order_item (
    order_id int NOT NULL REFERENCES "order"(id) ON DELETE CASCADE,
    product_id int NOT NULL REFERENCES product(id),
    quantity int NOT NULL CHECK (quantity > 0),
    unit_price numeric(10,2) NOT NULL,
    PRIMARY KEY (order_id, product_id)
);
CREATE INDEX ON "order"(customer_id);
CREATE INDEX ON order_item(product_id);
CREATE TABLE review (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    product_id int NOT NULL REFERENCES product(id),
    customer_id int NOT NULL REFERENCES customer(id),
    stars smallint NOT NULL CHECK (stars BETWEEN 1 AND 5),
    body text
);
CREATE VIEW order_total AS
    SELECT o.id, o.customer_id, o.status, sum(i.quantity * i.unit_price) AS total
    FROM "order" o JOIN order_item i ON i.order_id = o.id GROUP BY o.id;
CREATE FUNCTION customer_spend(p_customer int) RETURNS numeric LANGUAGE sql STABLE AS $$
    SELECT coalesce(sum(total), 0) FROM shop.order_total WHERE customer_id = p_customer
$$;

INSERT INTO category (name) VALUES ('Books'), ('Music'), ('Garden'), ('Kitchen');
INSERT INTO category (name, parent_id) VALUES ('Poetry', 1), ('Vinyl', 2);
INSERT INTO product (category_id, sku, name, price, in_stock)
SELECT 1 + (g % 6), 'SKU-' || lpad(g::text, 5, '0'),
       (ARRAY['Heffalump trap','Honey pot','Pooh sticks','Hundred Acre map','Balloon','Umbrella',
              'Thistle seeds','Tail ribbon'])[1 + g % 8] || ' ' || g,
       round((5 + random() * 95)::numeric, 2), (random() * 50)::int
FROM generate_series(1, 200) g;
INSERT INTO customer (email, full_name, country, created_at)
SELECT lower(n) || g || '@example.org', n || ' ' || s, (ARRAY['CZ','SK','DE','PL','AT','GB'])[1 + g % 6],
       now() - (g || ' days')::interval
FROM generate_series(1, 500) g,
     LATERAL (SELECT (ARRAY['Anna','Jan','Eva','Petr','Marie','Tomas','Lucie','Karel'])[1 + g % 8] n,
                     (ARRAY['Novak','Svoboda','Dvorak','Cerny','Prochazka','Kucera'])[1 + g % 6] s) x;
INSERT INTO "order" (customer_id, ordered_at, status)
SELECT 1 + (g * 7) % 500, now() - ((g % 365) || ' days')::interval,
       (ARRAY['new','paid','shipped','shipped','cancelled'])[1 + g % 5]
FROM generate_series(1, 3000) g;
INSERT INTO order_item (order_id, product_id, quantity, unit_price)
SELECT o, p, 1 + (o + p) % 4, (SELECT price FROM product WHERE id = p)
FROM generate_series(1, 3000) o, LATERAL (SELECT DISTINCT 1 + ((o * k * 13) % 200) p FROM generate_series(1, 3) k) x;
INSERT INTO review (product_id, customer_id, stars, body)
SELECT 1 + g % 200, 1 + g % 500, 1 + g % 5, 'Review ' || g FROM generate_series(1, 800) g;
ANALYZE;
