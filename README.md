# TradingSystem
Repository of Computer Networking Project

## Overview
TradingSystem is a simple online trading platform prototype implemented as two native C++ applications:

- Frontend: a lightweight HTTP server that serves static HTML pages from `Frontend/public/` on `http://localhost:8081`
- Backend: a JSON HTTP API server that handles auth, account state, orders, matching, and persistence on `http://localhost:8080`

The backend uses SQLite for persistence and implements a simplified limit order book with price/time priority.

## Project Structure
- `Frontend/`
  - `public/`: static pages (index / login / register / markets / trade / portfolio)
  - `src/Main.cpp`: minimal HTTP file server (Winsock)
- `Backend/`
  - `src/server/`: HTTP parsing + router (Winsock)
  - `src/controllers/`: HTTP handlers (auth + trading endpoints)
  - `src/services/`: business logic (user auth, order placement, matching)
  - `src/database/`: SQLite access + schema initialization
  - `src/models/`: data models (user/order/trade)

## Requirements
- Windows
- Visual Studio Build Tools / Visual Studio with C++ toolchain
- CMake
- vcpkg (bootstrapped by `build.bat`)
- Git

## Quick Start (Windows)
From the repo root, double-click or run:

```
build.bat
```

This builds the Frontend and Backend and starts both applications in separate terminals.

Default ports:
- Backend API: `http://localhost:8080`
- Frontend UI: `http://localhost:8081`

Open `http://localhost:8081` in your browser to use the app.

## Configuration (.env)
At startup, the backend loads a `.env` file (searching upward from the current working directory).

```env
JWT_SECRET=change-me
DATABASE=tradingsystem.sqlite
```

- `JWT_SECRET`: HMAC secret used to sign/verify JWT tokens
- `DATABASE`: SQLite database file path (relative paths are resolved from where Backend is launched)

## Backend API
All responses are JSON unless stated otherwise.

### Auth / Identity
- `POST /register` (alias: `POST /auth/register`)
  - Body: `{ "username": "...", "password": "..." }`
  - Also accepts `application/x-www-form-urlencoded` and `multipart/form-data`
- `POST /login` (alias: `POST /auth/login`)
  - Body: `{ "username": "...", "password": "..." }`
  - Returns: `{ "token": "<jwt>" }`
  - Token expires after **1 hour**

Use the token for authenticated endpoints:

```
Authorization: Bearer <jwt>
```

### Trading + Account
- `POST /orders` (authenticated)
  - Body: `{ "symbol": "DBS", "side": "buy" | "sell", "price": 110.0, "qty": 10 }`
  - Returns: `{ "message": "order placed", "order_id": 123 }`
- `DELETE /orders` (authenticated)
  - Body: `{ "order_id": 123 }`
  - Cancels an open or partially-filled order owned by the calling user
  - Returns: `{ "message": "order cancelled" }`
- `GET /orders?symbol=DBS`
  - Returns the current open order book (buy + sell sides) for the given symbol
- `GET /trades?symbol=DBS`
  - Returns recent trade history for the given symbol
- `GET /my-orders` (authenticated)
  - Returns all orders (any status) placed by the logged-in user
- `GET /account` (authenticated)
  - Returns cash balance and holdings for the logged-in user
- `GET /positions` (authenticated)
  - Returns current asset holdings for the logged-in user
- `GET /markets`
  - Returns available trading symbols with latest prices and simulated market data
- `GET /snapshot`
  - Returns a real-time market snapshot (prices, volumes, best bid/ask per symbol)

## Matching / Pricing Model
The backend implements a simplified limit order book:
- Buy priority: higher price first; tie-breaker is earlier arrival
- Sell priority: lower price first; tie-breaker is earlier arrival
- Trade occurs when best buy price >= best sell price
- Execution price uses the resting-order rule (earlier arrival sets the price; midpoint fallback if simultaneous)
- All state updates (cash, holdings, order status, trade record) are executed atomically in a single SQLite transaction

## Database
SQLite tables are created automatically on startup if missing:
- `users`: username + password salt/hash (PBKDF2, 100k iterations)
- `accounts`: cash balance by username (default $100,000)
- `holdings`: asset quantities by (username, symbol)
- `orders`: order book with remaining quantity and status
- `trades`: executed trade history with timestamps

With WAL enabled, you may also see `-wal` and `-shm` sidecar files next to the DB file.

## Notes / Limitations
- Orders do not expire; use `DELETE /orders` to cancel manually.
- The server is single-threaded — concurrent requests are serialized.
- Market prices in `/markets` and `/snapshot` are simulated (not live data).
