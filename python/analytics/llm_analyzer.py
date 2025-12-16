#!/usr/bin/env python3
"""
LLM Analytics Sidecar for Titans Trading System

Integrates with local LLM (via Ollama/llama.cpp) to provide:
- Daily PnL attribution reports
- Trade analysis and insights
- Strategy performance summaries
- Anomaly detection explanations
"""

import json
import subprocess
import requests
from dataclasses import dataclass
from typing import List, Dict, Optional, Any
from datetime import datetime, timedelta
import os
import mmap
import struct

# Configuration
OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://localhost:11434")
DEFAULT_MODEL = os.environ.get("LLM_MODEL", "llama3")


@dataclass
class Trade:
    """Trade record"""
    timestamp: int
    symbol: str
    side: str
    price: float
    quantity: float
    pnl: float = 0.0


@dataclass
class Position:
    """Position snapshot"""
    symbol: str
    quantity: float
    avg_price: float
    current_price: float
    unrealized_pnl: float
    realized_pnl: float


@dataclass
class DailyStats:
    """Daily trading statistics"""
    date: str
    total_pnl: float
    realized_pnl: float
    unrealized_pnl: float
    num_trades: int
    win_rate: float
    max_drawdown: float
    sharpe_ratio: float
    positions: List[Position]
    trades: List[Trade]


class SharedMemoryReader:
    """Read telemetry data from shared memory (written by C++ engine)"""

    HEADER_FORMAT = "=QQQd"  # timestamp, num_positions, num_trades, total_pnl
    POSITION_FORMAT = "=16sdddd"  # symbol, qty, avg_price, current_price, unrealized_pnl
    TRADE_FORMAT = "=16sQcddd"  # symbol, timestamp, side, price, qty, pnl

    def __init__(self, shm_path: str = "/dev/shm/titans_telemetry"):
        self.shm_path = shm_path

    def read_latest(self) -> Optional[Dict[str, Any]]:
        """Read latest telemetry data from shared memory"""
        try:
            with open(self.shm_path, "rb") as f:
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

                # Read header
                header_size = struct.calcsize(self.HEADER_FORMAT)
                header = struct.unpack(self.HEADER_FORMAT, mm[:header_size])
                timestamp, num_positions, num_trades, total_pnl = header

                offset = header_size

                # Read positions
                positions = []
                pos_size = struct.calcsize(self.POSITION_FORMAT)
                for _ in range(num_positions):
                    pos_data = struct.unpack(
                        self.POSITION_FORMAT,
                        mm[offset:offset + pos_size]
                    )
                    symbol = pos_data[0].decode('utf-8').rstrip('\x00')
                    positions.append(Position(
                        symbol=symbol,
                        quantity=pos_data[1],
                        avg_price=pos_data[2],
                        current_price=pos_data[3],
                        unrealized_pnl=pos_data[4],
                        realized_pnl=0.0
                    ))
                    offset += pos_size

                # Read trades
                trades = []
                trade_size = struct.calcsize(self.TRADE_FORMAT)
                for _ in range(num_trades):
                    trade_data = struct.unpack(
                        self.TRADE_FORMAT,
                        mm[offset:offset + trade_size]
                    )
                    symbol = trade_data[0].decode('utf-8').rstrip('\x00')
                    trades.append(Trade(
                        timestamp=trade_data[1],
                        symbol=symbol,
                        side='BUY' if trade_data[2] == b'B' else 'SELL',
                        price=trade_data[3],
                        quantity=trade_data[4],
                        pnl=trade_data[5]
                    ))
                    offset += trade_size

                mm.close()

                return {
                    'timestamp': timestamp,
                    'total_pnl': total_pnl,
                    'positions': positions,
                    'trades': trades
                }

        except FileNotFoundError:
            return None
        except Exception as e:
            print(f"Error reading shared memory: {e}")
            return None


class LLMClient:
    """Client for interacting with local LLM (Ollama)"""

    def __init__(self, base_url: str = OLLAMA_URL, model: str = DEFAULT_MODEL):
        self.base_url = base_url
        self.model = model

    def generate(self, prompt: str, system_prompt: str = None) -> str:
        """Generate text using the LLM"""
        try:
            payload = {
                "model": self.model,
                "prompt": prompt,
                "stream": False
            }

            if system_prompt:
                payload["system"] = system_prompt

            response = requests.post(
                f"{self.base_url}/api/generate",
                json=payload,
                timeout=60
            )

            if response.status_code == 200:
                return response.json().get("response", "")
            else:
                return f"Error: {response.status_code} - {response.text}"

        except requests.exceptions.ConnectionError:
            return self._fallback_analysis(prompt)
        except Exception as e:
            return f"Error generating response: {e}"

    def _fallback_analysis(self, prompt: str) -> str:
        """Fallback when LLM is not available"""
        return """
[LLM Unavailable - Basic Analysis]

The local LLM service is not running. To enable AI-powered analysis:

1. Install Ollama: curl -fsSL https://ollama.com/install.sh | sh
2. Start Ollama: ollama serve
3. Pull model: ollama pull llama3

Basic metrics are still being tracked and logged.
"""


class TradingAnalyzer:
    """Analyzes trading performance using LLM"""

    SYSTEM_PROMPT = """You are an expert quantitative trading analyst.
Your task is to analyze trading performance data and provide actionable insights.
Be concise, data-driven, and focus on:
1. PnL attribution (what drove gains/losses)
2. Risk assessment (drawdown, volatility, exposure)
3. Strategy performance (win rate, profit factor, Sharpe)
4. Recommendations for improvement

Format your response in clear sections with bullet points.
Use numbers and percentages to support your analysis."""

    def __init__(self, llm_client: LLMClient = None):
        self.llm = llm_client or LLMClient()
        self.shm_reader = SharedMemoryReader()

    def generate_daily_report(self, stats: DailyStats) -> str:
        """Generate a daily PnL attribution report"""

        # Build prompt with trading data
        prompt = f"""
Analyze the following daily trading performance:

Date: {stats.date}

## Summary
- Total PnL: ${stats.total_pnl:,.2f}
- Realized PnL: ${stats.realized_pnl:,.2f}
- Unrealized PnL: ${stats.unrealized_pnl:,.2f}
- Number of Trades: {stats.num_trades}
- Win Rate: {stats.win_rate:.1%}
- Max Drawdown: {stats.max_drawdown:.1%}
- Sharpe Ratio: {stats.sharpe_ratio:.2f}

## Positions at End of Day
"""
        for pos in stats.positions:
            prompt += f"- {pos.symbol}: {pos.quantity:+.4f} @ ${pos.avg_price:,.2f} "
            prompt += f"(Current: ${pos.current_price:,.2f}, "
            prompt += f"Unrealized: ${pos.unrealized_pnl:+,.2f})\n"

        prompt += "\n## Trades\n"
        for trade in stats.trades[:20]:  # Limit to first 20 trades
            prompt += f"- {trade.side} {trade.quantity:.4f} {trade.symbol} "
            prompt += f"@ ${trade.price:,.2f} (PnL: ${trade.pnl:+,.2f})\n"

        if len(stats.trades) > 20:
            prompt += f"... and {len(stats.trades) - 20} more trades\n"

        prompt += """
Please provide:
1. PnL Attribution: What drove the gains/losses?
2. Risk Analysis: Any concerns about exposure or drawdown?
3. Strategy Performance: How is the strategy performing?
4. Recommendations: What should be adjusted?
"""

        return self.llm.generate(prompt, self.SYSTEM_PROMPT)

    def analyze_trade(self, trade: Trade, context: str = "") -> str:
        """Analyze a specific trade"""
        prompt = f"""
Analyze this trade:
- Symbol: {trade.symbol}
- Side: {trade.side}
- Quantity: {trade.quantity}
- Price: ${trade.price:,.2f}
- Timestamp: {datetime.fromtimestamp(trade.timestamp / 1e9)}
- PnL: ${trade.pnl:+,.2f}

Context: {context}

Was this a good trade? What could have been done better?
"""
        return self.llm.generate(prompt, self.SYSTEM_PROMPT)

    def explain_anomaly(self, anomaly_type: str, data: Dict[str, Any]) -> str:
        """Explain a detected anomaly"""
        prompt = f"""
An anomaly was detected in the trading system:

Type: {anomaly_type}
Data: {json.dumps(data, indent=2)}

Please explain:
1. What likely caused this anomaly?
2. What are the potential risks?
3. What action should be taken?
"""
        return self.llm.generate(prompt, self.SYSTEM_PROMPT)

    def get_live_insights(self) -> str:
        """Get real-time insights from shared memory data"""
        data = self.shm_reader.read_latest()
        if not data:
            return "No live data available. Ensure the trading engine is running."

        prompt = f"""
Current trading status (live):

Total PnL: ${data['total_pnl']:,.2f}
Active Positions: {len(data['positions'])}
Recent Trades: {len(data['trades'])}

Positions:
"""
        for pos in data['positions']:
            prompt += f"- {pos.symbol}: {pos.quantity:+.4f} (Unrealized: ${pos.unrealized_pnl:+,.2f})\n"

        prompt += "\nProvide a brief status update and any immediate concerns."

        return self.llm.generate(prompt, self.SYSTEM_PROMPT)


class ReportGenerator:
    """Generates formatted reports"""

    def __init__(self, analyzer: TradingAnalyzer):
        self.analyzer = analyzer

    def generate_html_report(self, stats: DailyStats, analysis: str) -> str:
        """Generate an HTML report"""
        html = f"""
<!DOCTYPE html>
<html>
<head>
    <title>Titans Daily Report - {stats.date}</title>
    <style>
        body {{ font-family: 'Segoe UI', sans-serif; margin: 40px; background: #1a1a2e; color: #eee; }}
        .header {{ background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); padding: 20px; border-radius: 10px; }}
        .metric {{ display: inline-block; margin: 10px 20px; text-align: center; }}
        .metric-value {{ font-size: 24px; font-weight: bold; }}
        .metric-label {{ font-size: 12px; color: #aaa; }}
        .positive {{ color: #4caf50; }}
        .negative {{ color: #f44336; }}
        .section {{ background: #16213e; padding: 20px; margin: 20px 0; border-radius: 10px; }}
        .analysis {{ white-space: pre-wrap; line-height: 1.6; }}
        table {{ width: 100%; border-collapse: collapse; }}
        th, td {{ padding: 10px; text-align: left; border-bottom: 1px solid #333; }}
    </style>
</head>
<body>
    <div class="header">
        <h1>Titans Trading Report</h1>
        <p>{stats.date}</p>
    </div>

    <div class="section">
        <h2>Performance Summary</h2>
        <div class="metric">
            <div class="metric-value {'positive' if stats.total_pnl >= 0 else 'negative'}">${stats.total_pnl:,.2f}</div>
            <div class="metric-label">Total PnL</div>
        </div>
        <div class="metric">
            <div class="metric-value">{stats.num_trades}</div>
            <div class="metric-label">Trades</div>
        </div>
        <div class="metric">
            <div class="metric-value">{stats.win_rate:.1%}</div>
            <div class="metric-label">Win Rate</div>
        </div>
        <div class="metric">
            <div class="metric-value">{stats.sharpe_ratio:.2f}</div>
            <div class="metric-label">Sharpe Ratio</div>
        </div>
    </div>

    <div class="section">
        <h2>AI Analysis</h2>
        <div class="analysis">{analysis}</div>
    </div>

    <div class="section">
        <h2>Positions</h2>
        <table>
            <tr><th>Symbol</th><th>Quantity</th><th>Avg Price</th><th>Current</th><th>Unrealized PnL</th></tr>
"""
        for pos in stats.positions:
            pnl_class = 'positive' if pos.unrealized_pnl >= 0 else 'negative'
            html += f"""
            <tr>
                <td>{pos.symbol}</td>
                <td>{pos.quantity:+.4f}</td>
                <td>${pos.avg_price:,.2f}</td>
                <td>${pos.current_price:,.2f}</td>
                <td class="{pnl_class}">${pos.unrealized_pnl:+,.2f}</td>
            </tr>
"""
        html += """
        </table>
    </div>
</body>
</html>
"""
        return html

    def save_report(self, stats: DailyStats, output_dir: str = "./reports"):
        """Generate and save daily report"""
        os.makedirs(output_dir, exist_ok=True)

        # Generate analysis
        analysis = self.analyzer.generate_daily_report(stats)

        # Generate HTML
        html = self.generate_html_report(stats, analysis)

        # Save files
        date_str = stats.date.replace("-", "")
        html_path = os.path.join(output_dir, f"report_{date_str}.html")
        json_path = os.path.join(output_dir, f"stats_{date_str}.json")

        with open(html_path, 'w') as f:
            f.write(html)

        with open(json_path, 'w') as f:
            json.dump({
                'date': stats.date,
                'total_pnl': stats.total_pnl,
                'realized_pnl': stats.realized_pnl,
                'unrealized_pnl': stats.unrealized_pnl,
                'num_trades': stats.num_trades,
                'win_rate': stats.win_rate,
                'max_drawdown': stats.max_drawdown,
                'sharpe_ratio': stats.sharpe_ratio,
                'analysis': analysis
            }, f, indent=2)

        print(f"Report saved to {html_path}")
        return html_path


def main():
    """Example usage"""
    print("Titans LLM Analytics Sidecar")
    print("=" * 40)

    # Create analyzer
    analyzer = TradingAnalyzer()

    # Example: Generate report with sample data
    sample_stats = DailyStats(
        date=datetime.now().strftime("%Y-%m-%d"),
        total_pnl=1523.45,
        realized_pnl=1234.56,
        unrealized_pnl=288.89,
        num_trades=47,
        win_rate=0.62,
        max_drawdown=0.034,
        sharpe_ratio=1.85,
        positions=[
            Position("BTCUSDT", 0.5, 45000.0, 45500.0, 250.0, 1000.0),
            Position("ETHUSDT", -2.0, 2500.0, 2480.0, 40.0, 200.0),
        ],
        trades=[
            Trade(1702000000000000000, "BTCUSDT", "BUY", 45000.0, 0.1, 50.0),
            Trade(1702001000000000000, "BTCUSDT", "SELL", 45100.0, 0.1, 10.0),
            Trade(1702002000000000000, "ETHUSDT", "SELL", 2500.0, 1.0, 30.0),
        ]
    )

    print("\nGenerating daily report...")
    report = analyzer.generate_daily_report(sample_stats)
    print("\n" + report)

    # Save HTML report
    generator = ReportGenerator(analyzer)
    generator.save_report(sample_stats)


if __name__ == "__main__":
    main()
