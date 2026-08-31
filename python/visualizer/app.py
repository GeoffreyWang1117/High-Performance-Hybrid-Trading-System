#!/usr/bin/env python3
"""
Titans Trading System Visualizer

Streamlit-based dashboard for:
- Real-time position monitoring
- PnL visualization
- Order book display
- Strategy performance metrics
- LLM-generated insights
"""

import streamlit as st
import pandas as pd
import subprocess

import numpy as np
import plotly.graph_objects as go
import plotly.express as px
from plotly.subplots import make_subplots
from datetime import datetime, timedelta
import json
import os
import sys

# Add parent directory to path for imports
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    from analytics.llm_analyzer import TradingAnalyzer, DailyStats, Position, Trade
except ImportError:
    TradingAnalyzer = None

# Page config
st.set_page_config(
    page_title="Titans Trading Dashboard",
    page_icon="📈",
    layout="wide",
    initial_sidebar_state="expanded"
)

# Custom CSS
st.markdown("""
<style>
    .metric-card {
        background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        padding: 20px;
        border-radius: 10px;
        text-align: center;
        color: white;
    }
    .positive { color: #4caf50 !important; }
    .negative { color: #f44336 !important; }
    .stMetric > div { background: #1e1e2e; padding: 10px; border-radius: 5px; }
</style>
""", unsafe_allow_html=True)


def generate_demo_data():
    """Generate synthetic data for the demo dashboard.

    Nothing here comes from a running engine. Every price, fill, position and
    book level below is an np.random draw with a fixed seed. The dashboard
    labels itself accordingly -- a screenshot of a P&L curve is indistinguishable
    from a real one, so the page has to say what it is.
    """
    np.random.seed(42)

    # Generate price data
    dates = pd.date_range(start='2024-01-01', periods=100, freq='1H')
    btc_prices = 45000 + np.cumsum(np.random.randn(100) * 100)
    eth_prices = 2500 + np.cumsum(np.random.randn(100) * 20)

    price_data = pd.DataFrame({
        'timestamp': dates,
        'BTC': btc_prices,
        'ETH': eth_prices
    })

    # Generate trade data
    trades = []
    pnl = 0
    for i in range(50):
        side = np.random.choice(['BUY', 'SELL'])
        symbol = np.random.choice(['BTCUSDT', 'ETHUSDT'])
        price = btc_prices[i*2] if 'BTC' in symbol else eth_prices[i*2]
        qty = np.random.uniform(0.01, 0.5)
        trade_pnl = np.random.uniform(-100, 150)
        pnl += trade_pnl
        trades.append({
            'timestamp': dates[i*2],
            'symbol': symbol,
            'side': side,
            'price': price,
            'quantity': qty,
            'pnl': trade_pnl,
            'cumulative_pnl': pnl
        })

    trades_df = pd.DataFrame(trades)

    # Generate position data
    positions = [
        {'symbol': 'BTCUSDT', 'quantity': 0.5, 'avg_price': 45000, 'current_price': btc_prices[-1], 'unrealized_pnl': (btc_prices[-1] - 45000) * 0.5},
        {'symbol': 'ETHUSDT', 'quantity': -2.0, 'avg_price': 2500, 'current_price': eth_prices[-1], 'unrealized_pnl': (2500 - eth_prices[-1]) * 2.0},
    ]
    positions_df = pd.DataFrame(positions)

    # Order book data
    mid_price = btc_prices[-1]
    book_data = {
        'bids': [(mid_price - i*10, np.random.uniform(0.1, 2.0)) for i in range(1, 11)],
        'asks': [(mid_price + i*10, np.random.uniform(0.1, 2.0)) for i in range(1, 11)]
    }

    return price_data, trades_df, positions_df, book_data


def _demo_stats_from(trades_df: pd.DataFrame) -> "DailyStats":
    """Summarize the synthetic trades on this page into a DailyStats.

    Every field traces back to the np.random draws in generate_demo_data(), so
    the numbers are internally consistent with the charts above them instead of
    being invented separately.
    """
    pnl = trades_df["pnl"] if "pnl" in trades_df else pd.Series(dtype=float)
    total = float(pnl.sum()) if len(pnl) else 0.0
    wins = float((pnl > 0).mean()) if len(pnl) else 0.0
    # Per-trade Sharpe, not annualized: annualizing a few dozen synthetic fills
    # would dress noise up as a track record.
    sharpe = float(pnl.mean() / pnl.std()) if len(pnl) > 1 and pnl.std() > 0 else 0.0
    curve = pnl.cumsum() if len(pnl) else pd.Series(dtype=float)
    if len(curve):
        peak = curve.cummax()
        drawdown = float(((peak - curve) / peak.replace(0, float("nan"))).max())
        if drawdown != drawdown:      # NaN
            drawdown = 0.0
    else:
        drawdown = 0.0

    return DailyStats(
        date=datetime.now().strftime("%Y-%m-%d"),
        total_pnl=total,
        realized_pnl=total,
        unrealized_pnl=0.0,
        num_trades=int(len(pnl)),
        win_rate=wins,
        max_drawdown=drawdown,
        sharpe_ratio=sharpe,
        positions=[],
        trades=[],
    )


def _detect_gpu() -> str:
    """Return a GPU description, or an empty string if none is present.

    Queries nvidia-smi rather than asserting availability.
    """
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0 and out.stdout.strip():
            names = [n.strip() for n in out.stdout.strip().splitlines() if n.strip()]
            if len(names) == 1:
                return names[0]
            return f"{len(names)}x {names[0]}"
    except (OSError, subprocess.SubprocessError):
        pass
    return ""


def render_header():
    """Render dashboard header."""
    st.error(
        "**DEMO DATA — nothing on this page comes from a running engine.** "
        "Prices, fills, positions and book levels are all `np.random` draws "
        "with a fixed seed. This page exists to show the layout, not results. "
        "Wiring it to a live engine is listed under Known Limitations in the "
        "README."
    )

    col1, col2, col3 = st.columns([2, 1, 1])

    with col1:
        st.title("🚀 Titans Trading Dashboard")

    with col2:
        st.metric("Status", "demo")

    with col3:
        st.write(f"Rendered: {datetime.now().strftime('%H:%M:%S')}")


def render_pnl_metrics(trades_df: pd.DataFrame, positions_df: pd.DataFrame):
    """Render PnL metrics cards"""
    st.subheader("📊 Performance Metrics")

    total_realized = trades_df['pnl'].sum()
    total_unrealized = positions_df['unrealized_pnl'].sum()
    total_pnl = total_realized + total_unrealized

    win_trades = len(trades_df[trades_df['pnl'] > 0])
    total_trades = len(trades_df)
    win_rate = win_trades / total_trades if total_trades > 0 else 0

    col1, col2, col3, col4, col5 = st.columns(5)

    with col1:
        st.metric("Total PnL", f"${total_pnl:,.2f}",
                  delta=f"${total_pnl:+,.2f}")

    with col2:
        st.metric("Realized PnL", f"${total_realized:,.2f}")

    with col3:
        st.metric("Unrealized PnL", f"${total_unrealized:,.2f}")

    with col4:
        st.metric("Trades", total_trades)

    with col5:
        st.metric("Win Rate", f"{win_rate:.1%}")


def render_pnl_chart(trades_df: pd.DataFrame):
    """Render cumulative PnL chart"""
    st.subheader("📈 Cumulative PnL")

    fig = go.Figure()

    fig.add_trace(go.Scatter(
        x=trades_df['timestamp'],
        y=trades_df['cumulative_pnl'],
        mode='lines',
        name='Cumulative PnL',
        line=dict(color='#667eea', width=2),
        fill='tozeroy',
        fillcolor='rgba(102, 126, 234, 0.2)'
    ))

    # Add zero line
    fig.add_hline(y=0, line_dash="dash", line_color="gray")

    fig.update_layout(
        template='plotly_dark',
        height=400,
        margin=dict(l=0, r=0, t=30, b=0),
        xaxis_title="Time",
        yaxis_title="PnL ($)",
        showlegend=False
    )

    st.plotly_chart(fig, use_container_width=True)


def render_price_chart(price_data: pd.DataFrame, symbol: str = 'BTC'):
    """Render price chart with indicators"""
    st.subheader(f"📉 {symbol} Price")

    prices = price_data[symbol].values
    dates = price_data['timestamp']

    # Calculate indicators
    sma_20 = pd.Series(prices).rolling(20).mean()
    sma_50 = pd.Series(prices).rolling(50).mean()

    # Bollinger Bands
    std_20 = pd.Series(prices).rolling(20).std()
    upper_band = sma_20 + 2 * std_20
    lower_band = sma_20 - 2 * std_20

    fig = make_subplots(rows=2, cols=1, shared_xaxes=True,
                        vertical_spacing=0.05, row_heights=[0.7, 0.3])

    # Price
    fig.add_trace(go.Scatter(x=dates, y=prices, name='Price',
                             line=dict(color='#4caf50')), row=1, col=1)
    fig.add_trace(go.Scatter(x=dates, y=sma_20, name='SMA 20',
                             line=dict(color='#ff9800', dash='dash')), row=1, col=1)
    fig.add_trace(go.Scatter(x=dates, y=upper_band, name='Upper BB',
                             line=dict(color='gray', dash='dot')), row=1, col=1)
    fig.add_trace(go.Scatter(x=dates, y=lower_band, name='Lower BB',
                             line=dict(color='gray', dash='dot'),
                             fill='tonexty', fillcolor='rgba(128,128,128,0.1)'), row=1, col=1)

    # Volume (simulated)
    volume = np.abs(np.random.randn(len(prices))) * 100
    colors = ['green' if prices[i] > prices[i-1] else 'red'
              for i in range(1, len(prices))]
    colors.insert(0, 'gray')

    fig.add_trace(go.Bar(x=dates, y=volume, name='Volume',
                         marker_color=colors), row=2, col=1)

    fig.update_layout(
        template='plotly_dark',
        height=500,
        margin=dict(l=0, r=0, t=30, b=0),
        showlegend=True,
        legend=dict(orientation='h', y=1.1)
    )

    st.plotly_chart(fig, use_container_width=True)


def render_order_book(book_data: dict):
    """Render order book visualization"""
    st.subheader("📚 Order Book")

    bids = book_data['bids']
    asks = book_data['asks']

    # Create DataFrame
    bid_df = pd.DataFrame(bids, columns=['price', 'quantity'])
    ask_df = pd.DataFrame(asks, columns=['price', 'quantity'])

    col1, col2 = st.columns(2)

    with col1:
        fig = go.Figure()
        fig.add_trace(go.Bar(
            x=bid_df['quantity'],
            y=bid_df['price'],
            orientation='h',
            name='Bids',
            marker_color='green'
        ))
        fig.update_layout(
            template='plotly_dark',
            height=300,
            title='Bids',
            xaxis_title='Quantity',
            yaxis_title='Price',
            showlegend=False
        )
        st.plotly_chart(fig, use_container_width=True)

    with col2:
        fig = go.Figure()
        fig.add_trace(go.Bar(
            x=ask_df['quantity'],
            y=ask_df['price'],
            orientation='h',
            name='Asks',
            marker_color='red'
        ))
        fig.update_layout(
            template='plotly_dark',
            height=300,
            title='Asks',
            xaxis_title='Quantity',
            yaxis_title='Price',
            showlegend=False
        )
        st.plotly_chart(fig, use_container_width=True)


def render_positions(positions_df: pd.DataFrame):
    """Render positions table"""
    st.subheader("📋 Positions")

    # Format DataFrame for display
    display_df = positions_df.copy()
    display_df['unrealized_pnl'] = display_df['unrealized_pnl'].apply(
        lambda x: f"{'🟢' if x >= 0 else '🔴'} ${x:+,.2f}"
    )
    display_df['quantity'] = display_df['quantity'].apply(lambda x: f"{x:+.4f}")
    display_df['avg_price'] = display_df['avg_price'].apply(lambda x: f"${x:,.2f}")
    display_df['current_price'] = display_df['current_price'].apply(lambda x: f"${x:,.2f}")

    st.dataframe(display_df, use_container_width=True, hide_index=True)


def render_trades(trades_df: pd.DataFrame):
    """Render recent trades table"""
    st.subheader("📜 Recent Trades")

    display_df = trades_df.tail(10).copy()
    display_df['pnl'] = display_df['pnl'].apply(
        lambda x: f"{'🟢' if x >= 0 else '🔴'} ${x:+,.2f}"
    )
    display_df['side'] = display_df['side'].apply(
        lambda x: f"{'🟢 BUY' if x == 'BUY' else '🔴 SELL'}"
    )
    display_df['price'] = display_df['price'].apply(lambda x: f"${x:,.2f}")
    display_df['quantity'] = display_df['quantity'].apply(lambda x: f"{x:.4f}")

    st.dataframe(
        display_df[['timestamp', 'symbol', 'side', 'price', 'quantity', 'pnl']],
        use_container_width=True,
        hide_index=True
    )


def render_llm_insights(trades_df: pd.DataFrame):
    """Render LLM-generated insights over this page's synthetic trades."""
    st.subheader("🤖 AI Insights")

    if TradingAnalyzer is None:
        st.warning("LLM Analytics module not available")
        return

    st.caption(
        "The figures below are computed from this page's synthetic trades. "
        "Asking a model to comment on them produces commentary about random "
        "numbers, which is worth reading as a demonstration of the prompt and "
        "nothing else."
    )

    if st.button("Generate Analysis"):
        with st.spinner("Generating AI analysis..."):
            try:
                analyzer = TradingAnalyzer()
                # Derived from the demo trades on this page rather than
                # hardcoded. The previous version passed fixed numbers --
                # total_pnl 1523.45, sharpe 1.85 -- so the model wrote
                # confident commentary about a P&L that existed nowhere, and
                # the output read as genuine analysis.
                stats = _demo_stats_from(trades_df)
                analysis = analyzer.generate_daily_report(stats)
                st.markdown(analysis)
            except Exception as e:
                st.error(f"Error generating analysis: {e}")


def render_sidebar():
    """Render sidebar with controls"""
    with st.sidebar:
        st.header("⚙️ Controls")

        # Symbol selector
        st.selectbox("Symbol", ["BTCUSDT", "ETHUSDT", "All"], key="symbol")

        # Time range
        st.selectbox("Time Range", ["1H", "4H", "1D", "1W", "1M"], key="timerange")

        # Refresh
        if st.button("🔄 Refresh Data"):
            st.rerun()

        st.divider()

        st.header("📊 Quick Stats")
        # Previously hardcoded to 2 / 5 / 47 regardless of anything on screen.
        st.metric("Active Positions", "—")
        st.metric("Open Orders", "—")
        st.metric("Today's Trades", "—")
        st.caption("No engine connected; see the banner above.")

        st.divider()

        st.header("🔧 System")
        # These used to be hardcoded successes -- "Engine: Running",
        # "Market Data: Connected", "GPU: Available" -- displayed whether or
        # not any of it was true. A status light that is always green is not a
        # status light. GPU presence is now actually queried; the other two are
        # marked unknown until the dashboard is wired to an engine.
        st.warning("Engine: not connected (demo mode)")
        st.warning("Market Data: not connected (demo mode)")

        gpu = _detect_gpu()
        if gpu:
            st.success(f"GPU: {gpu}")
        else:
            st.info("GPU: none detected")


def main():
    """Main application"""
    # Load data
    price_data, trades_df, positions_df, book_data = generate_demo_data()

    # Render components
    render_sidebar()
    render_header()

    # Tabs for different views
    tab1, tab2, tab3, tab4 = st.tabs(["📈 Overview", "📊 Analysis", "📚 Order Book", "🤖 AI Insights"])

    with tab1:
        render_pnl_metrics(trades_df, positions_df)
        render_pnl_chart(trades_df)

        col1, col2 = st.columns(2)
        with col1:
            render_positions(positions_df)
        with col2:
            render_trades(trades_df)

    with tab2:
        col1, col2 = st.columns(2)
        with col1:
            render_price_chart(price_data, 'BTC')
        with col2:
            render_price_chart(price_data, 'ETH')

    with tab3:
        render_order_book(book_data)

    with tab4:
        render_llm_insights(trades_df)


if __name__ == "__main__":
    main()
