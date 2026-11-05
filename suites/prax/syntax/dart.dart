// A counter that persists its value, and a widget-free test of it.
import 'dart:async';
import 'dart:math' as math;

abstract class Store {
  Future<int?> read(String key);
  Future<void> write(String key, int value);
}

class MemoryStore implements Store {
  final Map<String, int> _values = {};

  @override
  Future<int?> read(String key) async => _values[key];

  @override
  Future<void> write(String key, int value) async {
    _values[key] = value;
  }
}

class Counter {
  static const String key = 'counter';
  final Store store;
  int _value = 0;
  final _changes = StreamController<int>.broadcast();

  Counter(this.store);

  Stream<int> get changes => _changes.stream;
  int get value => _value;

  Future<void> load() async {
    _value = await store.read(key) ?? 0;
  }

  Future<void> add([int by = 1]) async {
    _value = math.max(0, _value + by);
    _changes.add(_value);
    await store.write(key, _value);
  }
}

Future<void> main() async {
  final counter = Counter(MemoryStore());
  await counter.load();
  counter.changes.listen((v) => print('value: $v, doubled: ${v * 2}'));
  for (var i = 0; i < 3; i++) {
    await counter.add();
  }
  assert(counter.value == 3, 'expected 3\n');
}
