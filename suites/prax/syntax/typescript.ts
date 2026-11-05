// A typed event bus whose handlers know the payload of each event.

export type Events = {
    "user:login": { id: string; at: Date };
    "user:logout": { id: string };
    "cart:update": { items: number; total: number };
};

type Handler<T> = (payload: T) => void | Promise<void>;

export class EventBus<E extends Record<string, unknown>> {
    private readonly handlers = new Map<keyof E, Set<Handler<any>>>();
    private static readonly MAX_HANDLERS = 32;

    on<K extends keyof E>(event: K, handler: Handler<E[K]>): () => void {
        let set = this.handlers.get(event);
        if (!set) {
            set = new Set();
            this.handlers.set(event, set);
        }
        if (set.size >= EventBus.MAX_HANDLERS) {
            throw new RangeError(`too many handlers for ${String(event)}`);
        }
        set.add(handler);
        return () => set!.delete(handler);
    }

    async emit<K extends keyof E>(event: K, payload: E[K]): Promise<number> {
        const set = this.handlers.get(event) ?? new Set();
        await Promise.all([...set].map((handler) => handler(payload)));
        return set.size;
    }
}

enum Level {
    Debug = 0,
    Info,
    Warn,
}

function isCart(payload: unknown): payload is Events["cart:update"] {
    return typeof payload === "object" && payload !== null && "items" in payload;
}

const bus = new EventBus<Events>();
const off = bus.on("cart:update", ({ items, total }) => {
    console.log(`cart: ${items} items, $${(total / 100).toFixed(2)}`);
});
await bus.emit("cart:update", { items: 3, total: 4_250 });
off();
export const level: Level = Level.Info;
export { isCart };
