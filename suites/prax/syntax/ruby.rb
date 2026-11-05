# frozen_string_literal: true

# An inventory that tracks stock by SKU and warns when an item runs low.
require "json"
require "logger"

module Shop
  class OutOfStock < StandardError; end

  class Inventory
    include Enumerable

    LOW_STOCK = 5
    Item = Struct.new(:sku, :name, :quantity, keyword_init: true)

    attr_reader :items

    def initialize(logger: Logger.new($stdout))
      @items = {}
      @logger = logger
    end

    def add(sku, name:, quantity: 0)
      item = (@items[sku] ||= Item.new(sku: sku, name: name, quantity: 0))
      item.quantity += quantity
      self
    end

    def take(sku, count = 1)
      item = @items.fetch(sku) { raise KeyError, "unknown SKU #{sku}" }
      raise OutOfStock, "only #{item.quantity} of #{item.name}" if item.quantity < count

      item.quantity -= count
      @logger.warn("#{item.name} is low: #{item.quantity}") if item.quantity <= LOW_STOCK
      item
    end

    def each(&block)
      @items.each_value(&block)
    end

    def to_json(*args)
      map(&:to_h).to_json(*args)
    end
  end
end

if $PROGRAM_NAME == __FILE__
  inventory = Shop::Inventory.new
  inventory.add(:mug, name: "Mug", quantity: 7).add(:pen, name: "Pen", quantity: 40)
  begin
    3.times { inventory.take(:mug) }
  rescue Shop::OutOfStock => e
    puts "failed: #{e.message}"
  end
  puts inventory.select { |item| item.quantity > 10 }.map(&:name).inspect
  puts inventory.to_json, /^[A-Z]\w+$/.match?("Mug"), 1_000, 3.14
end
