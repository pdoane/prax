// Command wordcount counts the words of its input concurrently, a file to a worker.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"sort"
	"strings"
	"sync"
)

const maxWorkers = 4

type count struct {
	word string
	n    int
}

var errEmpty = errors.New("no files")

func countFile(path string, totals map[string]int, mu *sync.Mutex) error {
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("open %q: %w", path, err)
	}
	defer f.Close()
	scanner := bufio.NewScanner(f)
	scanner.Split(bufio.ScanWords)
	for scanner.Scan() {
		word := strings.ToLower(strings.Trim(scanner.Text(), ".,;:!?\"'"))
		mu.Lock()
		totals[word]++
		mu.Unlock()
	}
	return scanner.Err()
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, errEmpty)
		os.Exit(2)
	}
	totals := make(map[string]int)
	var mu sync.Mutex
	var wg sync.WaitGroup
	slots := make(chan struct{}, maxWorkers)
	for _, path := range os.Args[1:] {
		wg.Add(1)
		slots <- struct{}{}
		go func(path string) {
			defer func() { <-slots; wg.Done() }()
			if err := countFile(path, totals, &mu); err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
		}(path)
	}
	wg.Wait()

	counts := make([]count, 0, len(totals))
	for word, n := range totals {
		counts = append(counts, count{word, n})
	}
	sort.Slice(counts, func(i, j int) bool { return counts[i].n > counts[j].n })
	for _, c := range counts[:min(10, len(counts))] {
		fmt.Printf("%6d\t%s\n", c.n, c.word)
	}
}
