
$('[href="#toggle-show"]').on('click', function(e) {
  e.preventDefault();
  $(e.target).closest('.toggle-top').find('table').toggleClass('hide');
});
