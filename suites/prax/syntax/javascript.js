// Debounces a search box and renders its results, cancelling a search that a newer one replaces.
import { escapeHtml } from "./escape.js";

const DELAY_MS = 200;
const endpoint = new URL("/api/search", location.origin);

/**
 * Returns a function that calls `fn` once `ms` have passed without another call.
 * @param {Function} fn
 * @param {number} ms
 */
export function debounce(fn, ms = DELAY_MS) {
    let timer = null;
    return (...args) => {
        clearTimeout(timer);
        timer = setTimeout(() => fn(...args), ms);
    };
}

class SearchBox {
    #controller = null;

    constructor(input, list) {
        this.input = input;
        this.list = list;
        input.addEventListener("input", debounce(() => this.search(input.value.trim())));
    }

    async search(query) {
        this.#controller?.abort();
        if (query.length < 2) {
            this.list.replaceChildren();
            return;
        }
        this.#controller = new AbortController();
        endpoint.searchParams.set("q", query);
        try {
            const response = await fetch(endpoint, { signal: this.#controller.signal });
            const { results = [] } = await response.json();
            this.list.innerHTML = results
                .map(({ title, score }) => `<li data-score="${score.toFixed(2)}">${escapeHtml(title)}</li>`)
                .join("\n");
        } catch (error) {
            if (error.name !== "AbortError") console.error(`search failed: ${error.message}`);
        }
    }
}

const box = new SearchBox(document.querySelector("#q"), document.querySelector("#results"));
export default box;
