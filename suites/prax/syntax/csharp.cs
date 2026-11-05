// An order book that matches bids against asks.
using System;
using System.Collections.Generic;
using System.Linq;

namespace Exchange;

public enum Side { Buy, Sell }

public sealed record Order(Guid Id, Side Side, decimal Price, int Quantity);

public class OrderBook
{
    private readonly SortedDictionary<decimal, Queue<Order>> _bids = new(Comparer<decimal>.Create((a, b) => b.CompareTo(a)));
    private readonly SortedDictionary<decimal, Queue<Order>> _asks = new();

    public event EventHandler<string>? Traded;

    public int Depth => _bids.Values.Sum(q => q.Count) + _asks.Values.Sum(q => q.Count);

    public void Place(Order order)
    {
        var book = order.Side == Side.Buy ? _bids : _asks;
        if (!book.TryGetValue(order.Price, out var level))
        {
            level = new Queue<Order>();
            book[order.Price] = level;
        }
        level.Enqueue(order);
        Match();
    }

    private void Match()
    {
        while (_bids.Count > 0 && _asks.Count > 0)
        {
            var (bidPrice, bids) = _bids.First();
            var (askPrice, asks) = _asks.First();
            if (bidPrice < askPrice)
                break;
            var bid = bids.Dequeue();
            var ask = asks.Dequeue();
            Traded?.Invoke(this, $"traded {Math.Min(bid.Quantity, ask.Quantity)} @ {askPrice:F2}");
            if (bids.Count == 0) _bids.Remove(bidPrice);
            if (asks.Count == 0) _asks.Remove(askPrice);
        }
    }

    public static void Main()
    {
        var book = new OrderBook();
        book.Traded += (_, message) => Console.WriteLine(message);
        book.Place(new Order(Guid.NewGuid(), Side.Sell, 101.50m, 10));
        book.Place(new Order(Guid.NewGuid(), Side.Buy, 102.00m, 4));
        Console.WriteLine($"depth: {book.Depth}\tdone");
    }
}
