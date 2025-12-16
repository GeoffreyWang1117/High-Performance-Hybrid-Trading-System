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


def load_sample_data():
    """Load sample data for demonstration"""
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


def render_header():
    """Render dashboard header"""
    col1, col2, col3 = st.columns([2, 1, 1])

    with col1:
        st.title("🚀 Titans Trading Dashboard")

    with col2:
        st.metric("Status", "🟢 Running")

    with col3:
        st.write(f"Last Update: {datetime.now().strftime('%H:%M:%S')}")


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


def render_llm_insights():
    """Render LLM-generated insights"""
    st.subheader("🤖 AI Insights")

    if TradingAnalyzer is None:
        st.warning("LLM Analytics module not available")
        return

    if st.button("Generate Analysis"):
        with st.spinner("Generating AI analysis..."):
            try:
                analyzer = TradingAnalyzer()
                # Create sample stats
                stats = DailyStats(
                    date=datetime.now().strftime("%Y-%m-%d"),
                    total_pnl=1523.45,
                    realized_pnl=1234.56,
                    unrealized_pnl=288.89,
                    num_trades=47,
                    win_rate=0.62,
                    max_drawdown=0.034,
                    sharpe_ratio=1.85,
                    positions=[],
                    trades=[]
                )
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
        st.metric("Active Positions", "2")
        st.metric("Open Orders", "5")
        st.metric("Today's Trades", "47")

        st.divider()

        st.header("🔧 System")
        st.success("Engine: Running")
        st.success("Market Data: Connected")
        st.info("GPU: Available")


def main():
    """Main application"""
    # Load data
    price_data, trades_df, positions_df, book_data = load_sample_data()

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
        render_llm_insights()


if __name__ == "__main__":
    main()
