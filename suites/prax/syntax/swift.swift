// A weather client over async/await, with a cache that an actor guards.
import Foundation

enum WeatherError: Error, CustomStringConvertible {
    case badStatus(Int)
    case decoding(String)

    var description: String {
        switch self {
        case .badStatus(let code): return "server answered \(code)"
        case .decoding(let reason): return "could not decode: \(reason)"
        }
    }
}

struct Forecast: Decodable, Sendable {
    let city: String
    let celsius: Double
    var fahrenheit: Double { celsius * 9 / 5 + 32 }
}

actor ForecastCache {
    private var entries: [String: (Forecast, Date)] = [:]
    private let lifetime: TimeInterval = 600

    func get(_ city: String) -> Forecast? {
        guard let (forecast, stored) = entries[city], Date().timeIntervalSince(stored) < lifetime else {
            return nil
        }
        return forecast
    }

    func put(_ forecast: Forecast) {
        entries[forecast.city] = (forecast, Date())
    }
}

final class WeatherClient {
    private let base = URL(string: "https://api.example.com/v1/forecast")!
    private let cache = ForecastCache()

    func forecast(for city: String) async throws -> Forecast {
        if let cached = await cache.get(city) {
            return cached
        }
        var components = URLComponents(url: base, resolvingAgainstBaseURL: false)!
        components.queryItems = [URLQueryItem(name: "city", value: city)]
        let (data, response) = try await URLSession.shared.data(from: components.url!)
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw WeatherError.badStatus(http.statusCode)
        }
        let forecast = try JSONDecoder().decode(Forecast.self, from: data)
        await cache.put(forecast)
        return forecast
    }
}

@main
struct Main {
    static func main() async {
        do {
            let forecast = try await WeatherClient().forecast(for: "Oslo")
            print("\(forecast.city): \(forecast.celsius)°C\t\(String(format: "%.1f", forecast.fahrenheit))°F")
        } catch {
            print("failed: \(error)")
        }
    }
}
